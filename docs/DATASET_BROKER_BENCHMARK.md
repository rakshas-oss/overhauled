# Dataset-driven BRK2 broker benchmarks

Overhauled consumes the same version-1 dataset manifest used by YuKKi-OS, but
it does not own the dataset semantics or numerical algorithms. YuKKi-OS owns
manifest interpretation, deterministic data profiles, and the eigensolver.
This adapter submits the manifest's declared numeric records through the
existing native Overhauled BRK2 broker so broker ingress, dispatch, concurrency,
and end-to-end response latency can be measured on representative multivariate
workloads.

The adapter does not implement an eigensolver, transform the source dataset,
or change BRK1/BRK2/NXR1/WSM1 wire layouts. Each source row is converted into a
bounded BRK2 request payload containing the sequence number, timestamp, and
declared feature values as network-order IEEE-754 doubles. The broker's
execution preference defaults to `cpu`; `gpu` and `npu` can be requested
when those backends are configured and supported by the running broker.

## Run

Build and start the native broker using the normal Overhauled instructions.
Then, from the repository root:

```sh
python3 benchmarks/dataset_broker_bench.py \\
  --manifest ../YuKKi-OS/examples/benchmark-datasets/manifest.json \\
  --host 127.0.0.1 \\
  --port 9090 \\
  --device cpu \\
  --dataset all \\
  --profile all \\
  --output benchmark-results/dataset-broker.json
```

Use `--dry-run` to validate manifest references and inspect candidate
workloads without contacting a broker. Only `latency` and `throughput`
profiles are broker workload objectives. Memory and geospatial objectives
remain YuKKi-OS measurements; they are not relabelled as broker results.

For each dataset/profile pair, the adapter runs the declared warmup records,
then deterministically selects the requested sample. Every batch-size and
worker-count combination is run against the same selected records. The report
contains dataset and manifest SHA-256 digests, request count, throughput,
mean/P50/P95/P99 request latency, candidate configurations, selected
configuration, and whether an optional `max_p95_request_ms` constraint was
met. Use the same broker host, hardware, broker configuration, dataset
revision, and profile to compare runs. Every response is decoded and checked for
BRK2 envelope/schema validity, request-ID and task-ID correlation, successful
status, backend identity, and consistency between backend and selected_gpu.
Reports include actual backend counts, selected GPU IDs, and CPU fallback
counts; a requested device preference is never treated as proof of execution.

## Ownership boundary

- **YuKKi-OS:** manifest contract, dataset mapping and validation, numerical
  eigensolver, eigensolver accuracy metrics, and spatial/geometric analytics.
- **Overhauled:** native BRK2 broker execution, CPU/GPU/NPU placement, queueing,
  and end-to-end request measurements.

The adapter sends ordinary BRK2 inference tasks; it does not create a private
protocol or add a runtime dependency on YuKKi-OS. This is a native broker
qualification benchmark, not execution of the browser-WASM spatial module.
The browser-WASM runner can optionally submit the same host-side records to
the broker for a live BRK2 round trip; its local-only mode does not measure
broker interoperability. The manifest is a shared
file-format contract, not a cross-repository library dependency.

## Interpretation and safety

The included financial, drone, and environmental records are synthetic
fixtures. They test data plumbing and processing behavior, not live market
quality, environmental accuracy, or flight safety. A `gpu` or `npu`
preference is only a request; use the broker response and backend telemetry to
confirm actual execution placement. Request latency alone is not proof of
hardware selection or acceleration.

Do not expose the unauthenticated broker listener to an untrusted network.
The script has bounded manifest, dataset, row, feature, frame, response,
batch, worker, and socket-timeout limits. It never reads secrets from the
manifest.
