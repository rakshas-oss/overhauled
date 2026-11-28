> **Release status:** the multivariate_stats library is part of the published
> v1.11.0 release (2026-11-26). v1.11.2 is a metadata/documentation-only patch
> candidate; it does not change the library implementation or protocol.

# Multivariate stream aggregation

Overhauled exposes `Overhauled::multivariate_stats`, a CPU-only C++17
library for bounded-memory, online aggregation of normalized numeric
observations produced by YuKKi-OS or other trusted ingestion adapters.

## Contract

The accumulator supports at most 32 dimensions. Each observation supplies a
fixed 32-value array and a bit mask identifying present dimensions. Missing
values are omitted from the relevant pairwise calculation; they are never
converted to zero. Non-finite present values and bits outside the configured
dimension range are rejected.

For every dimension pair, the implementation maintains pairwise sample count,
online means and a co-moment. Snapshots expose population covariance and
Pearson correlation. Correlation is zero when there are insufficient samples
or either dimension has zero variance. Pairwise deletion means different
matrix cells may have different sample counts; consumers should inspect
`pair_counts` before interpreting a matrix.

## Parallel and distributed reduction

Use one accumulator per worker or NUMA shard. Merge shards at a controlled
barrier; no shared hot lock is introduced. Merges require identical dimension
counts and check all pair counters for overflow before mutation. Memory use is
fixed by the 32-dimension ceiling, independent of stream length.

## Build and test

```sh
cmake -S . -B build-cpu -DBUILD_TESTING=ON -DENABLE_CUDA=OFF
cmake --build build-cpu --target multivariate_stats_test --parallel
ctest --test-dir build-cpu --output-on-failure -R multivariate_stats_test
```

This library accepts normalized numeric observations only. It does not fetch
URLs, parse untrusted documents, execute code, retain raw payloads, or imply
GPU/NPU dispatch. YuKKi-OS remains responsible for source admission, bounded
transport, provenance, parsing and tenant quotas. Accelerator support can be
added later behind explicit capability detection and CPU-equivalence tests.
