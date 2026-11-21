# Overhauled v1.3.0 (release candidate)

> **Release candidate v1.3.0:** extends the published v1.2.0 worker fabric with validated hierarchical CPU-group/NUMA placement, class-local leaf queues, hierarchical work stealing, dynamic CPU masks and bounded large-host worker capacity.

> **Route workloads by topology, not guesswork.**
>
> Turn GPU interconnect awareness into higher throughput, better tail latency, and more effective use of accelerator infrastructure.

> **Release status:** v1.3.0 is a minor-release candidate based on the published v1.2.0 release. Required CI, review, release tag/archive and external package-manager submissions are pending.

Overhauled is a C++17 placement and execution library with an interoperable broker service. It provides topology-aware GPU scheduling and broker dispatch for CPU, GPU, and optional NPU execution.

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)](#)
[![Release](https://img.shields.io/badge/Release-v1.3.0-blueviolet)](#)
[![Broker](https://img.shields.io/badge/Protocol-BRK1%2FBRK2-orange)](#)

## Capabilities

- NVLink-aware topology detection, sticky home-GPU affinity, and queue-aware placement.
- Heuristic backend dispatch: forced/safety-sensitive work remains CPU-first; eligible NPU inference requires an available executor, and data-parallel GPU work must meet the configured offload threshold.
- Broker responses report the backend actually selected; a BRK2 device preference is a request, not proof of hardware execution.
- BRK1/v1 compatibility and BRK2/v2 TLV framing on the same TCP listener.
- A shared Rust/C++ protocol conformance corpus and CPU-only test suite.
- Opt-in mainline Linux DRM Ethos-U85 executor, with a documented Vela-generated `YU85BND1` bundle format.
- Optional CUDA/TensorRT GPU runtime features; deterministic CPU-only broker operation remains available.
- Isolated Control, RealtimeSpatial, Throughput, ExactCpu and Inference worker pools with bounded process-wide admission and in-flight memory budgets.
- Deadline-aware spatial queue ordering, dynamically sized Linux CPU masks, nested NUMA/CPU-group placement, and BRK2 queue/service/worker telemetry.
- Per-class, per-CPU-group queue locks isolate scheduler contention;
  realtime deadline insertion uses ordered leaf-queue entries, and worker CPU
  affinity is stable within the worker's validated CPU-group assignment.
- Worker queues are sharded by LLC/CPU group within each process-visible NUMA
  node. Workers prefer their home group, steal within the NUMA parent next, and
  cross NUMA domains only as a fallback.
- Topology validation enforces machine cpuset -> NUMA node -> CPU-group
  containment, sibling disjointness, and bounded depth/fanout. Dynamic CPU masks
  support CPU IDs beyond the legacy CPU_SETSIZE boundary.
- Large-host worker limits are 16,384 total (4,096 elastic); automatic sizing
  targets physical cores and is capped at 4,096 workers.

## v1.2.0 published feature baseline

The published v1.2.0 release added a broker-side worker fabric coordinated with
YuKKi-OS workload directives:

- Five worker classes: Control, RealtimeSpatial, Throughput, ExactCpu and
  Inference, each with an isolated queue and reserved worker pool.
- RealtimeSpatial requests use earliest-deadline-first ordering when an
  absolute deadline is present; equal deadlines retain admission order.
- Process-wide outstanding-request and in-flight-memory budgets remain held
  until execution completes, including after client disconnect.
- Linux topology discovery builds a bounded machine -> NUMA -> LLC/CPU-group
  tree from the process affinity mask and sysfs, validates every child cpuset
  against its parent, and uses dynamically sized worker affinity masks.
  Telemetry reports a NUMA node only when affinity was successfully applied.
- Optional BRK2 request fields 11–13 carry worker class, logical spatial
  affinity and preferred NUMA node. Optional response fields 8–12 report
  worker class, queue wait, service time, worker ID and applied NUMA node.
- The dataset benchmark can exercise class, affinity, NUMA and deadline
  behavior and report queue/service measurements.
- Per-class queues are sharded across eligible CPU groups inside NUMA nodes. Optional elastic
  workers use weighted RealtimeSpatial/Throughput/Inference service with
  process-wide per-class ceilings; Control and ExactCpu remain dedicated.

BRK1 remains on the legacy Throughput queue and preserves its backend
selection semantics. Existing BRK1/BRK2/WSM1 envelope identities and required
fields are unchanged. Deadlines are broker request bounds, not preemption or
hard real-time guarantees. See
[YuKKi-aware worker fabric](docs/YUKKI_WORKER_FABRIC.md).

## What's new in v1.3.0

The v1.3.0 candidate adds a hierarchical worker fabric for large CPU hosts.
It builds on the worker-class and deadline features in v1.2.0:

- **Nested topology:** validates the process-allowed machine CPU set, NUMA
  parents and LLC/CPU-group leaf domains. Child sets must remain inside their
  parent, sibling groups must be disjoint, and nested groups must cover the
  allowed set without widening the process cpuset.
- **Large CPU masks:** dynamic Linux affinity masks support CPU IDs beyond the
  legacy `CPU_SETSIZE` boundary. Invalid, duplicate, malformed or excessive
  CPU-list/topology input is rejected.
- **Hierarchical queue placement:** each worker class has ordered per-CPU-group
  leaf queues. Workers prefer their home group, then steal within sibling
  groups in the same NUMA node, and cross NUMA domains only as a fallback.
  Work stealing preserves worker class and does not change task semantics.
- **Bounded large-host capacity:** explicit sizing supports up to 16,384 total
  workers, including at most 4,096 elastic workers. Automatic sizing targets
  discovered physical cores and is capped at 4,096; operators should still
  measure stack memory and workload behavior before increasing limits.
- **Lower scheduling contention:** per-class/per-group queues isolate locks
  and wakeups; admission accounting uses atomic reservations instead of one
  global admission mutex.
- **Benchmark coverage:** adds repeated A/B throughput runs, pipelined
  mixed-class saturation, measurement-quality checks and fixed worker-budget
  sweeps. These provide reproducible measurements, not a universal scaling
  or throughput guarantee.

The existing BRK1/BRK2/WSM1 contracts are preserved. BRK2 worker intent and
telemetry fields introduced in v1.2.0 remain unchanged; the CPU-group ID is
internal scheduling detail, not a new wire field. NUMA affinity and work
stealing remain bounded by the process-visible CPU set and actual OS support.
See [YuKKi-aware worker fabric](docs/YUKKI_WORKER_FABRIC.md) and
[architecture](docs/ARCHITECTURE.md).

## Throughput and energy-efficiency model

The documented four-GPU A100 benchmark at eight concurrent clients reports 87.3 ops/s for NVLink-aware placement versus 58.4 ops/s for blind round-robin placement—approximately **49.5% higher throughput** in that workload.

A conditional energy-per-operation estimate can be derived from those throughput figures: **if average total system power is equal in both runs**, the higher-throughput configuration would use approximately **33.1% less energy per completed operation** (1 − 58.4 / 87.3). This is a normalized model, not a measured power reduction or a guaranteed electricity-bill saving. The benchmark does not report wall power, GPU-board power, host power, workload duty cycle, or facility overhead. Actual OPEX depends on measured system power, operating hours, utilization, and the local electricity tariff.

For a deployment, estimate annual energy cost as:

`annual kWh = average system kW × operating hours`

`annual electricity cost = annual kWh × tariff per kWh`

Compare both schedulers on the same hardware and workload while recording wall power and completed work. See [docs/BENCHMARK_RESULTS.md](docs/BENCHMARK_RESULTS.md) for the benchmark context.

## Backend selection and operational boundaries

The broker selects a backend before GPU placement. Forced CPU and unverified
exact-precision, verification-sensitive, or cryptographic profiles remain on
CPU. NPU execution requires an inference profile, the service-side enable gate,
and an available concrete executor. `--npu-simulated` is CPU emulation for
conformance only; physical Ethos-U execution is an opt-in Linux DRM path with
a compatible Vela `YU85BND1` bundle. A BRK2 device preference requests a
backend; the response is the authoritative report of what actually ran.

The broker's wire preference is not a substitute for a task's real semantics.
CPU retry after accelerator failure is only safe for operations that are
idempotent and explicitly marked fallback-safe. Review
[Architecture](docs/ARCHITECTURE.md) before changing dispatch policy.

## Control-plane and WASM boundary

YuKKi-OS remains the control plane and Wasmtime guest host; Overhauled owns
broker execution, protocol dispatch and accelerator placement. The repositories
communicate through BRK1/BRK2 and the documented WSM1 lifecycle/task envelope.
Overhauled does not validate, compile, link, compose or instantiate guest Wasm
modules. Its Prepare/Drain/Release operations track broker-side version and
in-flight task state. YuKKi-OS performs guest compilation and multi-module
program composition. See [WASM interoperability](docs/WASM_INTEROP.md) and
[YuKKi-OS composition guide](https://github.com/rakshas-oss/YuKKi-OS/blob/main/docs/WASM_PROGRAM_COMPOSITION.md).

## Cross-repository multivariate benchmark bridge

There are two complementary benchmark entry points, with different scopes:

- YuKKi-OS's browser runner executes its local spatial module in Node's
  WebAssembly runtime. When broker mode is enabled, it additionally submits
  host-prepared numeric fixture records directly to this broker over BRK2.
- This repository's dataset broker runner reads the shared version-1 dataset
  manifest and measures native broker ingress, dispatch, worker/batch
  concurrency, response latency, and actual backend telemetry.

Neither path sends executable Wasm modules to this broker. The browser
benchmark leg is ordinary BRK2 inference traffic, not a WSM1 guest task or
lifecycle operation. YuKKi-OS remains authoritative for manifest semantics,
spatial processing, resolver-derived locality hints, and the eigensolver.
Overhauled owns broker dispatch, worker-class queues, process-local Linux
NUMA discovery/CPU affinity, GPU topology/placement, and CPU/GPU/NPU execution.
The two repositories therefore keep workload-level topology semantics
separate from broker-local worker placement.

See [Dataset-driven BRK2 broker benchmarks](docs/DATASET_BROKER_BENCHMARK.md),
[YuKKi-OS benchmarking guide](https://github.com/rakshas-oss/YuKKi-OS/blob/main/docs/BENCHMARKING.md),
and [YuKKi-OS interoperability](docs/YUKKI_INTEROP.md).


## Architecture map — full-colour ANSI palette

![Overhauled broker and execution architecture diagram](docs/diagrams/overhauled-architecture.svg)

Overhauled is the broker and execution tier. It validates BRK1/BRK2 frames,
applies bounded admission, routes requests into five isolated worker-class
queues with nested CPU-group/NUMA shards, performs broker-local Linux
CPU-group/NUMA affinity where supported, and uses hierarchical work stealing
(home group -> sibling groups -> remote NUMA) with bounded class-aware elasticity
before dispatching eligible work to CPU/GPU/NPU backends. It does not duplicate
YuKKi-OS workload semantics or spatial resolution: YuKKi selects the class
and sends an optional affinity key, deadline and preferred-NUMA hint;
Overhauled resolves that hint against its own process-allowed CPU topology and
reports the NUMA node only if thread affinity succeeds. BRK2 field 7 reports
the actual backend; optional fields 8–12 report worker and placement telemetry.
WSM1 remains task/lifecycle metadata, not a Wasm runtime; Wasmtime compilation,
guest isolation and execution remain on the YuKKi-OS host.

The accompanying terminal sketch uses 24-bit ANSI colours:
`bash docs/diagrams/print-ansi.sh`. GitHub does not render terminal escape
sequences inside Markdown code blocks, so the README embeds the equivalent
full-colour SVG.


## Quick start: CPU-only broker

```bash
git clone https://github.com/rakshas-oss/overhauled.git
cd overhauled
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_CUDA=OFF \
  -DENABLE_TENSORRT=OFF \
  -DBUILD_BROKER=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
./build/broker_server 9090 --cpu-only
```

This mode is useful for CI, development, and non-GPU hosts. The service uses bounded broker framing, but the protocol is not an authentication layer; deploy it only on a trusted network or behind an authenticated transport.

## Optional Ethos-U85 Linux DRM backend

Physical NPU execution is opt-in and requires Linux with a kernel exposing the mainline Ethos-U DRM UAPI, plus a compatible Vela-generated bundle. Configure and build with:

```bash
cmake -S . -B build -DENABLE_ETHOSU_DRM=ON -DENABLE_CUDA=OFF -DENABLE_TENSORRT=OFF -DBUILD_BROKER=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Then launch using a `YU85BND1` model bundle:

```bash
./build/broker_server 9090 --npu-ethosu-bundle model.yu85
./build/broker_server 9090 --npu-ethosu-bundle model.yu85 --ethosu-device /dev/dri/renderD128
```

`--npu-simulated` is CPU emulation for CI/conformance, not hardware acceleration. See [docs/ETHOSU_U85_BUNDLE.md](docs/ETHOSU_U85_BUNDLE.md).

## Other build modes

```bash
# CUDA-aware placement, without TensorRT
cmake -S . -B build -DENABLE_CUDA=ON -DENABLE_TENSORRT=OFF

# TensorRT runtime server (requires CUDA and user-provided TensorRT)
cmake -S . -B build -DENABLE_CUDA=ON -DENABLE_TENSORRT=ON -DTENSORRT_ROOT=/path/to/tensorrt
```

## Security hardening

Placement validates non-empty GPU topology, model registry access is synchronized, and the optional TensorRT runtime enforces bounded framed input handling. The BRK2 response encoder caps error text to configured limits. These checks do not provide transport authentication. Review [docs/SECURITY_HARDENING_NOTES.md](docs/SECURITY_HARDENING_NOTES.md) and deploy network services behind an authenticated transport boundary.

## Documentation

- [Broker protocol](docs/BROKER_PROTOCOL.md) · [BRK2 conformance](docs/BRK2_CONFORMANCE_V1.md) · [Executable BRK corpus](docs/BRK_CONFORMANCE_V1.md)
- [YuKKi-OS interoperability](docs/YUKKI_INTEROP.md) · [WASM interoperability](docs/WASM_INTEROP.md)
- [Ethos-U85 bundle and hardware boundary](docs/ETHOSU_U85_BUNDLE.md) · [YuKKi-aware worker fabric](docs/YUKKI_WORKER_FABRIC.md)
- [Architecture and runtime decision model](docs/ARCHITECTURE.md) · [Heuristic and backend-selection verification](docs/SYSADMIN_HOWTO.md) · [Versioning](docs/VERSIONING.md)
- [Architecture and decision model](docs/ARCHITECTURE.md) · [Library guide](docs/LIBRARY.md) · [Versioning](docs/VERSIONING.md)
- [System administration step-by-step](docs/SYSADMIN_HOWTO.md) · [Operations runbook](OPERATIONS_RUNBOOK.md) · [Deployment guide](DEPLOYMENT_GUIDE.md)
- [Publishing](docs/PUBLISHING.md) · [Release notes](docs/RELEASE_NOTES.md) · [Benchmarks](docs/BENCHMARK_RESULTS.md) · [Dataset-driven BRK2 workloads](docs/DATASET_BROKER_BENCHMARK.md)
- [Security hardening notes](docs/SECURITY_HARDENING_NOTES.md)

## License

MIT License — see [LICENSE](LICENSE). Optional CUDA Toolkit and TensorRT components are user-supplied proprietary NVIDIA software, not redistributed under this project's license; see [NOTICE](NOTICE).


## YuKKi-OS service discovery

YuKKi-OS can advertise reachable Overhauled broker endpoints through its authenticated peer mesh using bounded lease-based gossip. This does not make the broker a direct DHT participant; BRK1/BRK2 transport remains independently secured. See [YUKKI_BROKER_DISCOVERY.md](docs/YUKKI_BROKER_DISCOVERY.md).
