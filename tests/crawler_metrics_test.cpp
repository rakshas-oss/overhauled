#include "crawler_metrics.h"

#include <cassert>
#include <cmath>
#include <cstdint>

using nvlink::crawler::MetricAccumulator;
using nvlink::crawler::PageObservation;

int main() {
    MetricAccumulator first;
    assert(first.observe(PageObservation{10, 2, 1, 2, 200}));
    assert(first.observe(PageObservation{20, 3, 4, 8, 201}));
    assert(first.observe(PageObservation{0, 0, 8, 0, 503}));

    const auto snapshot = first.snapshot();
    assert(snapshot.pages_total == 3);
    assert(snapshot.pages_succeeded == 2);
    assert(snapshot.pages_failed == 1);
    assert(snapshot.total_document_bytes == 30);
    assert(snapshot.total_links == 5);
    assert(snapshot.fetch.count == 3);
    assert(snapshot.fetch.min_us == 1);
    assert(snapshot.fetch.max_us == 8);
    assert(std::fabs(snapshot.fetch.mean_us - (13.0 / 3.0)) < 1e-9);
    assert(snapshot.fetch.p50_bucket_upper_us == 4);
    assert(snapshot.fetch.p95_bucket_upper_us == 8);
    assert(snapshot.extraction.count == 3);
    assert(snapshot.extraction.min_us == 0);
    assert(snapshot.extraction.max_us == 8);

    MetricAccumulator second;
    assert(second.observe(PageObservation{100, 7, 16, 32, 200}));
    assert(second.observe(PageObservation{50, 1, 2, 4, 429}));

    assert(first.merge(second));
    const auto merged = first.snapshot();
    assert(merged.pages_total == 5);
    assert(merged.pages_succeeded == 3);
    assert(merged.pages_failed == 2);
    assert(merged.total_document_bytes == 180);
    assert(merged.total_links == 13);
    assert(merged.fetch.count == 5);
    assert(merged.fetch.max_us == 16);
    assert(merged.extraction.max_us == 32);

    first.reset();
    const auto empty = first.snapshot();
    assert(empty.pages_total == 0);
    assert(empty.fetch.count == 0);
    assert(empty.fetch.p95_bucket_upper_us == 0);
    return 0;
}
