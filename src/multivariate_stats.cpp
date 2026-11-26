#include "multivariate_stats.h"

#include <cmath>
#include <limits>

namespace nvlink::stream_stats {
namespace {
bool can_add(std::uint64_t a, std::uint64_t b) noexcept {
    return b <= std::numeric_limits<std::uint64_t>::max() - a;
}
}

MultivariateAccumulator::MultivariateAccumulator(std::size_t dimensions) noexcept
    : dimensions_(dimensions <= kMaxDimensions ? dimensions : 0) {}

bool MultivariateAccumulator::observe(
    const std::array<double, kMaxDimensions>& values,
    std::uint32_t present_mask) noexcept {
    if (dimensions_ == 0) return false;
    const std::uint32_t valid_mask = dimensions_ == 32
        ? std::numeric_limits<std::uint32_t>::max()
        : ((std::uint32_t{1} << dimensions_) - 1U);
    if ((present_mask & ~valid_mask) != 0U) return false;
    for (std::size_t i = 0; i < dimensions_; ++i) {
        if ((present_mask & (std::uint32_t{1} << i)) != 0U
            && !std::isfinite(values[i])) return false;
    }

    for (std::size_t i = 0; i < dimensions_; ++i) {
        if ((present_mask & (std::uint32_t{1} << i)) == 0U) continue;
        for (std::size_t j = 0; j < dimensions_; ++j) {
            if ((present_mask & (std::uint32_t{1} << j)) == 0U) continue;
            const auto& pair = pairs_[i * kMaxDimensions + j];
            if (!can_add(pair.count, 1)) return false;
        }
    }

    for (std::size_t i = 0; i < dimensions_; ++i) {
        if ((present_mask & (std::uint32_t{1} << i)) == 0U) continue;
        for (std::size_t j = 0; j < dimensions_; ++j) {
            if ((present_mask & (std::uint32_t{1} << j)) == 0U) continue;
            auto& pair = pairs_[i * kMaxDimensions + j];
            const double x = values[i];
            const double y = values[j];
            const double next_count = static_cast<double>(pair.count + 1);
            const double dx = x - pair.mean_x;
            const double dy = y - pair.mean_y;
            pair.mean_x += dx / next_count;
            pair.mean_y += dy / next_count;
            const double dy_after = y - pair.mean_y;
            pair.co_moment += dx * dy_after;
            pair.m2_x += dx * (x - pair.mean_x);
            pair.m2_y += dy * dy_after;
            ++pair.count;
        }
    }
    return true;
}

bool MultivariateAccumulator::merge(const MultivariateAccumulator& other) noexcept {
    if (this == &other || dimensions_ == 0 || dimensions_ != other.dimensions_) return false;
    for (std::size_t i = 0; i < dimensions_; ++i) {
        for (std::size_t j = 0; j < dimensions_; ++j) {
            const auto index = i * kMaxDimensions + j;
            if (!can_add(pairs_[index].count, other.pairs_[index].count)) return false;
        }
    }
    for (std::size_t i = 0; i < dimensions_; ++i) {
        for (std::size_t j = 0; j < dimensions_; ++j) {
            const auto index = i * kMaxDimensions + j;
            auto& target = pairs_[index];
            const auto& source = other.pairs_[index];
            if (source.count == 0) continue;
            if (target.count == 0) {
                target = source;
                continue;
            }
            const auto combined = target.count + source.count;
            const double a = static_cast<double>(target.count);
            const double b = static_cast<double>(source.count);
            const double n = static_cast<double>(combined);
            const double dx = source.mean_x - target.mean_x;
            const double dy = source.mean_y - target.mean_y;
            target.co_moment += source.co_moment + dx * dy * a * b / n;
            target.m2_x += source.m2_x + dx * dx * a * b / n;
            target.m2_y += source.m2_y + dy * dy * a * b / n;
            target.mean_x += dx * b / n;
            target.mean_y += dy * b / n;
            target.count = combined;
        }
    }
    return true;
}

MultivariateSummary MultivariateAccumulator::snapshot() const noexcept {
    MultivariateSummary result;
    result.dimensions = dimensions_;
    for (std::size_t i = 0; i < dimensions_; ++i) {
        for (std::size_t j = 0; j < dimensions_; ++j) {
            const auto index = i * kMaxDimensions + j;
            const auto& pair = pairs_[index];
            result.pair_counts[index] = pair.count;
            if (pair.count == 0) continue;
            result.covariance[index] = pair.co_moment / static_cast<double>(pair.count);
            // Use variances from this exact pair's jointly-present samples;
            // marginal diagonal counts may differ under pairwise deletion.
            if (pair.m2_x > 0.0 && pair.m2_y > 0.0) {
                const double denominator = std::sqrt(
                    pair.m2_x / static_cast<double>(pair.count)
                    * pair.m2_y / static_cast<double>(pair.count));
                result.correlation[index] = result.covariance[index] / denominator;
            }
        }
    }
    return result;
}

void MultivariateAccumulator::reset() noexcept {
    pairs_.fill(PairStats{});
}

} // namespace nvlink::stream_stats
