# Worker-fabric saturation benchmark

The mixed-class benchmark compares three broker configurations:

1. **Baseline:** the pre-NUMA/class-queue revision `accd334bd6964213cb6eb9c1eed295901b83b5e6`.
2. **Dedicated:** current `main`, with class-specific dedicated worker queues and no elastic threads.
3. **Elastic:** current `main`, with the same dedicated workers plus bounded elastic capacity for RealtimeSpatial, Throughput, and Inference.

The benchmark is diagnostic. It must not be used to claim a performance gain from one unusually fast trial.

## Measurement protocol

The workflow uses Release binaries and an identical BRK2 request workload for all arms:

- 5% Control, 20% RealtimeSpatial, 50% Throughput, 5% ExactCpu, and 20% Inference.
- 1,024 binary64 features per request.
- 8 and 32 concurrent TCP clients.
- Pipeline depths 1 and 32.
- 64 warm-up requests per client and configuration, excluded from measured results.
- 256 measured requests per client and configuration.
- Ten alternating repetitions per arm to reduce order and host-load bias.

The driver timestamps each request immediately before its individual socket write, then records the timestamp when the correlated response is received. Responses are correlated by BRK2 request ID, so out-of-order completion does not mix up latency measurements. Percentiles use the nearest-rank definition.

Queue wait and service time are taken from broker response telemetry. Client-observed latency includes request transmission, queueing, execution, and response receipt; it is not a pure service-time metric.

The Linux resource sampler reads process CPU-time counters and resident pages from `/proc/<pid>/stat`. The report derives interval CPU utilization as a percentage of one logical CPU (values can exceed 100% for a multi-threaded process) and reports maximum sampled RSS. These are process-level estimates, not whole-machine energy or power measurements.

## Running it

Every pull request runs the lightweight Python validation job. The longer A/B measurement is manual:

1. Open **Actions → Worker Fabric Mixed-Class Saturation → Run workflow**.
2. Choose `standard` to use GitHub-hosted Ubuntu. This is suitable for repeatable CPU/workload comparisons, but its NUMA topology may expose only one node.
3. Choose `numa2` only when the repository has an online self-hosted runner labelled `self-hosted`, `linux`, `x64`, and `numa`, with at least two NUMA nodes visible inside the runner process's CPU affinity mask.

The `numa2` profile fails early when fewer than two nodes are visible. It also fails after the run unless RealtimeSpatial, Throughput, and Inference worker telemetry demonstrates execution on at least two distinct NUMA nodes in both the dedicated and elastic configurations.

The workflow builds the fixed historical baseline and the current `origin/main` revision. Run it against `main` when the goal is to measure the merged implementation.

## Interpreting results

Compare medians across all ten repetitions, and read the median absolute deviation (MAD) percentage alongside the throughput values. A large MAD means the environment is too noisy for a strong conclusion. Inspect p95/p99 latency and class-specific queue wait before considering throughput alone.

The benchmark exercises CPU fallback and a synthetic vector transform. It does not establish GPU/NPU, WASM, geospatial, real application, power, or production workload performance. A one-node result cannot establish physical multi-node NUMA benefits. Keep production scheduler policy unchanged unless the measurements are stable and the improvements are repeatable under representative workload and topology.
