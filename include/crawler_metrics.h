#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace nvlink::crawler {

// One bounded observation emitted by YuKKi-OS after a page fetch and sandboxed
// extraction attempt. Durations are monotonic elapsed microseconds.
struct PageObservation {
    std::uint64_t document_bytes{0};
    std::uint64_t link_count{0};
    std::uint64_t fetch_duration_us{0};
    std::uint64_t extraction_duration_us{0};
    std::uint16_t status_code{0};
};

struct DurationSummary {
    std::uint64_t count{0};
    std::uint64_t min_us{0};
    std::uint64_t max_us{0};
    double mean_us{0.0};
    double standard_deviation_us{0.0};
    // Log2 histogram upper bounds; these deliberately overestimate the
    // percentile's bucket rather than claiming an exact sample percentile.
    std::uint64_t p50_bucket_upper_us{0};
    std::uint64_t p95_bucket_upper_us{0};
    std::uint64_t p99_bucket_upper_us{0};
};

struct MetricSummary {
    std::uint64_t pages_total{0};
    std::uint64_t pages_succeeded{0};
    std::uint64_t pages_failed{0};
    std::uint64_t total_document_bytes{0};
    std::uint64_t total_links{0};
    DurationSummary fetch;
    DurationSummary extraction;
};

// Constant-memory streaming aggregator. Keep one accumulator per worker/NUMA
// shard and merge at a barrier to avoid serializing hot metric producers.
class MetricAccumulator {
public:
    MetricAccumulator() = default;

    // Returns false on counter overflow. No field is modified on overflow.
    bool observe(const PageObservation& observation) noexcept;

    // Merge another shard into this one. Returns false without mutation if
    // any integer counter would overflow.
    bool merge(const MetricAccumulator& other) noexcept;

    MetricSummary snapshot() const noexcept;
    void reset() noexcept;

private:
    struct RunningStats {
        std::uint64_t count{0};
        std::uint64_t min{0};
        std::uint64_t max{0};
        double mean{0.0};
        double sum_squared_deviations{0.0};
    };

    static std::size_t bucket_for(std::uint64_t value) noexcept;
    static std::uint64_t percentile_upper(
        const std::array<std::uint64_t, 64>& histogram,
        std::uint64_t count,
        std::uint32_t percentile) noexcept;
    static bool can_add(std::uint64_t left, std::uint64_t right) noexcept;
    static void update_stats(RunningStats& stats, std::uint64_t value) noexcept;
    static void merge_stats(RunningStats& target, const RunningStats& source) noexcept;
    static DurationSummary duration_snapshot(
        const RunningStats& stats,
        const std::array<std::uint64_t, 64>& histogram) noexcept;

    std::uint64_t pages_total_{0};
    std::uint64_t pages_succeeded_{0};
    std::uint64_t pages_failed_{0};
    std::uint64_t total_document_bytes_{0};
    std::uint64_t total_links_{0};
    RunningStats fetch_stats_;
    RunningStats extraction_stats_;
    std::array<std::uint64_t, 64> fetch_histogram_{};
    std::array<std::uint64_t, 64> extraction_histogram_{};
};

} // namespace nvlink::crawler
