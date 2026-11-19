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

To exercise YuKKi-directed worker scheduling, add an explicit class and (for
real-time spatial work) a logical affinity key:

```sh
python3 benchmarks/dataset_broker_bench.py \
  --manifest ../YuKKi-OS/examples/benchmark-datasets/manifest.json \
  --host 127.0.0.1 --port 9090 \
  --device cpu \
  --worker-class realtime-spatial \
  --spatial-affinity-key 0x53454e534f522d31 \
  --preferred-numa-node 1 \
  --deadline-ms 250 \
  --dataset all --profile all \
  --output benchmark-results/realtime-spatial.json
```

Supported worker classes are `control`, `realtime-spatial`, `throughput`,
`exact-cpu`, and `inference`. The default is `throughput`. A real-time
spatial run requires `--spatial-affinity-key`; the NUMA node is an advisory
local hint and is reported only if the broker successfully applies Linux CPU
affinity. `--deadline-ms` creates an absolute per-request deadline from the
time each request is constructed. It is a broker deadline, not an end-to-end
deadline guarantee for the dataset profile.

When the broker returns worker telemetry, the benchmark records class,
queue-wait and service-time aggregates, worker IDs and applied NUMA nodes.
Older brokers may ignore the optional request TLVs and omit telemetry; such
runs retain backend and end-to-end measurements but do not qualify the new
worker fabric. Compare `worker_telemetry_requests` with total request count
before interpreting queue-wait metrics.

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

## Browser-WASM counterpart

The YuKKi-OS browser runner is a separate client-side measurement. It builds
and executes the spatial module in Node's WebAssembly runtime, then optionally
submits host-prepared numeric records from the same multivariate fixture to
this broker using direct BRK2 inference requests. From the YuKKi-OS checkout
root, the live mode is:

~~~sh
node browser-wasm/bench/run.mjs --quick --broker-host 127.0.0.1 --broker-port 9090 --broker-device cpu --output browser-wasm-brk2.json
~~~

This is not execution of the spatial WASM module inside Overhauled and is not
a WSM1 task/lifecycle test. It does not test browser rendering, gateway
streaming, NUMA scheduling, or physical accelerator hardware. For the
production guest lifecycle boundary, see [WASM interoperability](WASM_INTEROP.md).

## Client-side resource bounds

The Python benchmark applies the following guardrails before or during a run:

| Input or resource | Limit |
|---|---:|
| Manifest file | 1 MiB |
| Dataset file | 512 MiB |
| JSONL record | 64 KiB |
| Records per dataset | 1,000,000 |
| Numeric feature fields | 1–64 |
| Response frame | 16 MiB |
| Batch size | 1–1,024 |
| Worker count | 1–32 |
| Socket timeout | 0.1–120 seconds |

These are benchmark-client limits, not replacements for the broker's own
protocol limits. The broker may reject a request that satisfies the client
bounds if its configured frame or payload ceiling is lower.


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
