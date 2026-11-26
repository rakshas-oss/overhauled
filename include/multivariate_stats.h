#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace nvlink::stream_stats {

constexpr std::size_t kMaxDimensions = 32;

// Pairwise-deletion online statistics. A bit set in present_mask marks a
// present value; absent dimensions are not interpreted as zero.
struct MultivariateSummary {
    std::size_t dimensions{0};
    std::array<std::uint64_t, kMaxDimensions * kMaxDimensions> pair_counts{};
    std::array<double, kMaxDimensions * kMaxDimensions> covariance{};
    std::array<double, kMaxDimensions * kMaxDimensions> correlation{};
};

class MultivariateAccumulator {
public:
    explicit MultivariateAccumulator(std::size_t dimensions) noexcept;

    // Rejects invalid dimensions, out-of-range mask bits, non-finite present
    // values, and counter overflow without changing state.
    bool observe(const std::array<double, kMaxDimensions>& values,
                 std::uint32_t present_mask) noexcept;

    // Shards can be merged at a barrier. Dimensions must match. Self-merge is
    // rejected; integer overflow is checked before any mutation.
    bool merge(const MultivariateAccumulator& other) noexcept;

    MultivariateSummary snapshot() const noexcept;
    void reset() noexcept;
    std::size_t dimensions() const noexcept { return dimensions_; }

private:
    struct PairStats {
        std::uint64_t count{0};
        double mean_x{0.0};
        double mean_y{0.0};
        double co_moment{0.0};
        double m2_x{0.0};
        double m2_y{0.0};
    };

    std::size_t dimensions_{0};
    std::array<PairStats, kMaxDimensions * kMaxDimensions> pairs_{};
};

} // namespace nvlink::stream_stats
