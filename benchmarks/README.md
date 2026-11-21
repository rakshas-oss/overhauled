# Worker-fabric saturation benchmark

The mixed-class benchmark compares five configurations:

1. **Baseline:** the pre-NUMA/class-queue revision `accd334bd6964213cb6eb9c1eed295901b83b5e6`.
2. **Dedicated:** current `main`, with class-specific dedicated worker queues and no elastic threads.
3. **Elastic-1:** current `main`, with a total shared elastic pool capped at one thread.
4. **Elastic-2:** current `main`, with a total shared elastic pool capped at two threads.
5. **Elastic-4:** current `main`, with a total shared elastic pool capped at four threads.

The three elastic budgets share capacity across RealtimeSpatial, Throughput, and Inference. Each eligible class's individual ceiling is set to the selected budget, while `--workers-elastic` enforces the broker-wide total cap. The budget is therefore not multiplied by the number of eligible classes.

The benchmark is diagnostic. It must not be used to claim a performance gain from one unusually fast trial.

## Measurement protocol

The workflow uses Release binaries and an identical BRK2 request workload for all arms:

- 5% Control, 20% RealtimeSpatial, 50% Throughput, 5% ExactCpu, and 20% Inference.
- 1,024 binary64 features per request.
- 8 and 32 concurrent TCP clients.
- Pipeline depths 1 and 32.
- 64 warm-up requests per client and configuration, excluded from measured results.
- 256 measured requests per client and configuration.
- Ten repetitions per arm, rotating the order of all five configurations on each repetition to reduce order and host-load bias.
- The same dedicated worker counts are used for the dedicated and elastic arms; only the total elastic pool budget changes.

The driver timestamps each request immediately before its individual socket write, then records the timestamp when the correlated response is received. Responses are correlated by BRK2 request ID, so out-of-order completion does not mix up latency measurements. Percentiles use the nearest-rank definition.

Queue wait and service time are taken from broker response telemetry. Client-observed latency includes request transmission, queueing, execution, and response receipt; it is not a pure service-time metric.

The Linux resource sampler reads process CPU-time counters and resident pages from `/proc/<pid>/stat`. The report derives interval CPU utilization as a percentage of one logical CPU (values can exceed 100% for a multi-threaded process) and reports maximum sampled RSS. These are process-level estimates, not whole-machine energy or power measurements.

## Running it

Every pull request runs the lightweight Python validation job. The longer A/B measurement is manual:

1. Open **Actions → Worker Fabric Mixed-Class Saturation → Run workflow**.
2. Choose `standard` to use GitHub-hosted Ubuntu. This is suitable for repeatable CPU/workload comparisons, but its NUMA topology may expose only one node.
3. Choose `numa2` only when the repository has an online self-hosted runner labelled `self-hosted`, `linux`, `x64`, and `numa`, with at least two NUMA nodes visible inside the runner process's CPU affinity mask.

The `numa2` profile fails early when fewer than two nodes are visible. It also fails after the run unless RealtimeSpatial, Throughput, and Inference worker telemetry demonstrates execution on at least two distinct NUMA nodes in both the dedicated-only configuration and the largest elastic budget (four threads). The one- and two-thread budgets are still measured on NUMA runners, but are not independently required to span nodes because their smaller shared pools may legitimately remain local to one node.

The workflow builds the fixed historical baseline and the current `origin/main` revision. Run it against `main` when the goal is to measure the merged implementation.

## Interpreting results

Compare medians across all ten repetitions, and read the median absolute deviation (MAD) percentage alongside the throughput values. The summary reports total throughput for baseline, dedicated-only, and elastic budgets 1/2/4; percentage deltas for each budget against dedicated-only; p95 queue wait by class at 32 clients/pipeline depth 32; and process CPU/RSS medians. A large MAD means the environment is too noisy for a strong conclusion. Inspect p95/p99 latency and class-specific queue wait before considering throughput alone.

The benchmark exercises CPU fallback and a synthetic vector transform. It does not establish GPU/NPU, WASM, geospatial, real application, power, or production workload performance. A one-node result cannot establish physical multi-node NUMA benefits. Keep production scheduler policy unchanged unless the measurements are stable and the improvements are repeatable under representative workload and topology.
