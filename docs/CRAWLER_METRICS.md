# Crawler metric aggregation

Overhauled provides a constant-memory C++ accumulator for page observations
produced by the YuKKi-OS WASM Crawler Fabric. It is a library for embedding in
a metrics service; it does not open crawler connections, parse HTML, or add a
new BRK1/BRK2 message type.

## Build and test

~~~sh
cmake -S . -B build-cpu \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_BROKER=ON \
  -DBUILD_TESTING=ON \
  -DENABLE_CUDA=OFF \
  -DENABLE_TENSORRT=OFF
cmake --build build-cpu --target crawler_metrics_test --parallel
ctest --test-dir build-cpu --output-on-failure -R crawler_metrics_test
~~~

The installed library target is Overhauled::crawler_metrics and the public
header is crawler_metrics.h.

## Observation contract

Map one YuKKi-OS page record into one PageObservation:

~~~cpp
#include "crawler_metrics.h"

nvlink::crawler::MetricAccumulator shard;

const nvlink::crawler::PageObservation observation{
    /* document_bytes */ 4096,
    /* link_count */ 18,
    /* fetch_duration_us */ 12000,
    /* extraction_duration_us */ 840,
    /* status_code */ 200,
};
if (!shard.observe(observation)) {
    // Counter overflow: stop or rotate the shard; do not discard silently.
}
~~~

Any 2xx status is counted as a successful page; other status codes are
failures. Every observation contributes its byte/link totals and duration
samples, including failed requests when the caller has timing information.

## Parallel aggregation

The accumulator is deliberately not internally synchronized. Keep one
accumulator per worker or NUMA shard, then merge at a controlled barrier:

~~~cpp
nvlink::crawler::MetricAccumulator merged;
merged.merge(shard_a);
merged.merge(shard_b);
const auto summary = merged.snapshot();
~~~

Observation and merge use fixed-size counters and histograms. Memory use does
not grow with the number of pages. Counter overflow returns false before
mutating the accumulator.

## Statistics and interpretation

The snapshot reports page counts, total bytes, total links, and fetch/extraction
duration counts, minima, maxima, means and population standard deviations.
P50/P95/P99 values are upper bounds from a 64-bucket base-2 microsecond
histogram. They are bounded-memory approximations, not exact quantiles. For
strict quantiles, export bounded samples to a separate offline analysis path.

This initial library runs on CPU and does not wire the accumulator into the
broker request protocol or dispatch reductions to GPU/NPU. It deliberately
keeps raw HTML out of accelerator queues. Future broker integration must define
a versioned numeric-observation contract, preserve per-tenant admission and
memory quotas, and report actual backend selection rather than treating a
requested GPU/NPU preference as proof of execution.

## Security boundary

Page contents are never interpreted as executable code by this library. The
crawler's explicit source allowlist, DNS/egress controls and WASM guest limits
remain responsibilities of YuKKi-OS. This component accepts already extracted
numeric observations only.
