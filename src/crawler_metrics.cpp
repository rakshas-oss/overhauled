#include "crawler_metrics.h"

#include <cmath>
#include <limits>

namespace nvlink::crawler {

bool MetricAccumulator::can_add(std::uint64_t left, std::uint64_t right) noexcept {
    return right <= std::numeric_limits<std::uint64_t>::max() - left;
}

std::size_t MetricAccumulator::bucket_for(std::uint64_t value) noexcept {
    std::size_t bucket = 0;
    while (value > 1 && bucket < 63) {
        value >>= 1;
        ++bucket;
    }
    return bucket;
}

void MetricAccumulator::update_stats(RunningStats& stats, std::uint64_t value) noexcept {
    if (stats.count == 0) {
        stats.count = 1;
        stats.min = value;
        stats.max = value;
        stats.mean = static_cast<double>(value);
        stats.sum_squared_deviations = 0.0;
        return;
    }
    const double sample = static_cast<double>(value);
    const double delta = sample - stats.mean;
    ++stats.count;
    stats.mean += delta / static_cast<double>(stats.count);
    const double delta_after = sample - stats.mean;
    stats.sum_squared_deviations += delta * delta_after;
    if (value < stats.min) stats.min = value;
    if (value > stats.max) stats.max = value;
}

void MetricAccumulator::merge_stats(
    RunningStats& target, const RunningStats& source) noexcept {
    if (source.count == 0) return;
    if (target.count == 0) {
        target = source;
        return;
    }
    const auto combined = target.count + source.count;
    const double delta = source.mean - target.mean;
    const double target_count = static_cast<double>(target.count);
    const double source_count = static_cast<double>(source.count);
    const double combined_count = static_cast<double>(combined);
    target.sum_squared_deviations += source.sum_squared_deviations
        + delta * delta * target_count * source_count / combined_count;
    target.mean += delta * source_count / combined_count;
    target.count = combined;
    if (source.min < target.min) target.min = source.min;
    if (source.max > target.max) target.max = source.max;
}

bool MetricAccumulator::observe(const PageObservation& observation) noexcept {
    const bool success = observation.status_code >= 200 && observation.status_code < 300;
    const auto fetch_bucket = bucket_for(observation.fetch_duration_us);
    const auto extraction_bucket = bucket_for(observation.extraction_duration_us);

    if (!can_add(pages_total_, 1)
        || !can_add(success ? pages_succeeded_ : pages_failed_, 1)
        || !can_add(total_document_bytes_, observation.document_bytes)
        || !can_add(total_links_, observation.link_count)
        || !can_add(fetch_stats_.count, 1)
        || !can_add(extraction_stats_.count, 1)
        || !can_add(fetch_histogram_[fetch_bucket], 1)
        || !can_add(extraction_histogram_[extraction_bucket], 1)) {
        return false;
    }

    ++pages_total_;
    if (success) ++pages_succeeded_;
    else ++pages_failed_;
    total_document_bytes_ += observation.document_bytes;
    total_links_ += observation.link_count;
    update_stats(fetch_stats_, observation.fetch_duration_us);
    update_stats(extraction_stats_, observation.extraction_duration_us);
    ++fetch_histogram_[fetch_bucket];
    ++extraction_histogram_[extraction_bucket];
    return true;
}

bool MetricAccumulator::merge(const MetricAccumulator& other) noexcept {
    if (this == &other) return false;
    if (!can_add(pages_total_, other.pages_total_)
        || !can_add(pages_succeeded_, other.pages_succeeded_)
        || !can_add(pages_failed_, other.pages_failed_)
        || !can_add(total_document_bytes_, other.total_document_bytes_)
        || !can_add(total_links_, other.total_links_)
        || !can_add(fetch_stats_.count, other.fetch_stats_.count)
        || !can_add(extraction_stats_.count, other.extraction_stats_.count)) {
        return false;
    }
    for (std::size_t i = 0; i < fetch_histogram_.size(); ++i) {
        if (!can_add(fetch_histogram_[i], other.fetch_histogram_[i])
            || !can_add(extraction_histogram_[i], other.extraction_histogram_[i])) {
            return false;
        }
    }

    pages_total_ += other.pages_total_;
    pages_succeeded_ += other.pages_succeeded_;
    pages_failed_ += other.pages_failed_;
    total_document_bytes_ += other.total_document_bytes_;
    total_links_ += other.total_links_;
    merge_stats(fetch_stats_, other.fetch_stats_);
    merge_stats(extraction_stats_, other.extraction_stats_);
    for (std::size_t i = 0; i < fetch_histogram_.size(); ++i) {
        fetch_histogram_[i] += other.fetch_histogram_[i];
        extraction_histogram_[i] += other.extraction_histogram_[i];
    }
    return true;
}

std::uint64_t MetricAccumulator::percentile_upper(
    const std::array<std::uint64_t, 64>& histogram,
    std::uint64_t count,
    std::uint32_t percentile) noexcept {
    if (count == 0) return 0;
    const std::uint64_t rank = (count / 100) * percentile
        + ((count % 100) * percentile + 99) / 100;
    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < histogram.size(); ++i) {
        cumulative += histogram[i];
        if (cumulative >= rank) {
            return i == 63 ? std::numeric_limits<std::uint64_t>::max()
                           : (std::uint64_t{1} << i);
        }
    }
    return std::numeric_limits<std::uint64_t>::max();
}

DurationSummary MetricAccumulator::duration_snapshot(
    const RunningStats& stats,
    const std::array<std::uint64_t, 64>& histogram) noexcept {
    DurationSummary result;
    result.count = stats.count;
    if (stats.count == 0) return result;
    result.min_us = stats.min;
    result.max_us = stats.max;
    result.mean_us = stats.mean;
    result.standard_deviation_us = std::sqrt(
        stats.sum_squared_deviations / static_cast<double>(stats.count));
    result.p50_bucket_upper_us = percentile_upper(histogram, stats.count, 50);
    result.p95_bucket_upper_us = percentile_upper(histogram, stats.count, 95);
    result.p99_bucket_upper_us = percentile_upper(histogram, stats.count, 99);
    return result;
}

MetricSummary MetricAccumulator::snapshot() const noexcept {
    MetricSummary result;
    result.pages_total = pages_total_;
    result.pages_succeeded = pages_succeeded_;
    result.pages_failed = pages_failed_;
    result.total_document_bytes = total_document_bytes_;
    result.total_links = total_links_;
    result.fetch = duration_snapshot(fetch_stats_, fetch_histogram_);
    result.extraction = duration_snapshot(extraction_stats_, extraction_histogram_);
    return result;
}

void MetricAccumulator::reset() noexcept {
    pages_total_ = 0;
    pages_succeeded_ = 0;
    pages_failed_ = 0;
    total_document_bytes_ = 0;
    total_links_ = 0;
    fetch_stats_ = RunningStats{};
    extraction_stats_ = RunningStats{};
    fetch_histogram_.fill(0);
    extraction_histogram_.fill(0);
}

} // namespace nvlink::crawler
