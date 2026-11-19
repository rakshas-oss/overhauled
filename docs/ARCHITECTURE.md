# Architecture and Runtime Decision Model

## 1. System boundary

Overhauled is the execution and placement plane. It accepts bounded binary broker frames, validates and dispatches requests, selects an execution backend, and returns a structured result. It is designed to sit behind a separately authenticated control plane such as YuKKi-OS; it does not embed YuKKi-OS. NUMA topology discovery, NUMA-local memory policy, and NUMA worker scheduling are deliberately outside this repository; YuKKi-OS owns system topology/locality, while Overhauled's topology and placement code concern accelerator devices and GPU interconnects.

```text
YuKKi-OS / external client
        |
        | secured network or authenticated TCP proxy
        | raw TCP byte stream; no HTTP translation
        v
BrokerServer (default TCP port 9090)
        |
        +-- frame decoder and protocol limits
        |      +-- BRK1 / v1
        |      +-- BRK2 / v2 TLV
        |
        +-- kind-specific handlers
        |      +-- geospatial.frame.v1 (NXR1 validation / echo)
        |      +-- media.stream.v1 (MED1 validation / echo)
        |      +-- WASM task and lifecycle operations
        |
        +-- backend dispatcher (CPU / GPU / NPU)
               +-- CPU reference compute path
               +-- GPU compute path -> GpuTopology -> Placer
               +-- optional NPU executor
                       +-- simulated CPU executor (test only)
                       +-- optional Ethos-U85 Linux DRM executor
```

The broker is not an authentication, encryption, HTTP, or general-purpose model-serving layer. Put it on a trusted private network or behind an authenticated transport boundary.

### Full-colour system map

![Overhauled broker and execution architecture diagram](diagrams/overhauled-architecture.svg)

A terminal-renderable ANSI-colour version is available with
`bash docs/diagrams/print-ansi.sh`. The illustration distinguishes the
BRK1 compatibility response (no backend-identity field) from BRK2 response
field 7 (authoritative backend identity). It also shows the NPU simulator as
CPU emulation, not physical acceleration.


## 2. Transport and protocol boundary

Every TCP message is framed by a four-byte unsigned big-endian body length. The body magic selects BRK1/v1 or BRK2/v2; the same listener accepts both. The client chooses the format by sending its magic. There is no JSON HELLO negotiation.

- **BRK1/v1** is the compatibility path. It carries the established request contract but no backend-profile metadata. Without an operator-configured GPU default, profile-less BRK1 work uses CPU.
- **BRK2/v2** uses a bounded TLV envelope and request ID correlation. Its optional `device_preference` can request `cpu`, `gpu`, or `npu`; the response reports the backend actually selected. A request preference is not proof that hardware exists or that a hardware backend ran.
- Unknown critical fields, malformed encodings, duplicate singular fields, invalid lengths, and configured-limit violations are rejected.
- The protocol does not supply authentication, encryption, HTTP semantics, remote cancellation, or a negotiation handshake.

The full byte-level contract and limits live in [BROKER_PROTOCOL.md](BROKER_PROTOCOL.md), with cross-language vectors in [BRK_CONFORMANCE_V1.md](BRK_CONFORMANCE_V1.md) and [BRK2_CONFORMANCE_V1.md](BRK2_CONFORMANCE_V1.md).

## 3. Request classification and backend selection

Backend selection runs before GPU placement. The dispatcher is implemented in `include/backend_dispatch.h` and consumed by `BrokerService`.

The effective decision order is:

1. **Explicit CPU / forced CPU:** `--cpu-only` overrides all backend metadata. BRK2 `device_preference=cpu` also forces CPU for that request.
2. **Safety constraints:** exact-precision, verification-sensitive, or cryptographic profiles stay on CPU unless the profile explicitly declares an accelerator path verified.
3. **NPU-eligible inference:** inference work can use NPU only when NPU execution is enabled and a concrete executor reports available.
4. **GPU-eligible data-parallel work:** GPU selection requires an available GPU and data-parallel classification. The default byte threshold is 1 MiB; an optional FLOP threshold is available to in-process callers.
5. **CPU baseline:** requests that do not meet a selection rule run on CPU.

BRK2 `device_preference=gpu|npu` supplies a profile when the wire request has no in-process profile. GPU preference therefore still requires GPU availability; NPU preference still requires an enabled, available executor. If the requested accelerator is unavailable, the dispatcher uses CPU rather than claiming that accelerator execution occurred. The CLI accepts `--default-backend npu`, but the current profile-less default-selection rule only auto-offloads to GPU; use BRK2 `device_preference=npu` (or an in-process inference profile) to request NPU work.

Accelerator failure is retried on CPU only when the request is declared `fallback_safe`. Otherwise the broker returns an execution error. Do not mark a non-idempotent or side-effecting operation fallback-safe unless repeating it on CPU is genuinely safe.

## 4. GPU topology and placement

GPU placement is a second-stage decision and runs only after GPU backend selection.

- `GpuTopology` detects the available CUDA-visible GPUs and their peer-link relationships.
- `Placer` assigns a stable home GPU for a client source and uses queue-depth feedback to decide whether to retain that GPU, move to a preferred peer, or choose the least-loaded available GPU.
- The default backlog threshold is 32.
- Per-GPU in-flight accounting bounds admission. The broker tracks payload-weighted in-flight work for placement decisions while separately enforcing request-count limits.
- With no usable GPU topology, work stays on CPU; a selected GPU index of `-1` is the expected CPU result.

NVLink is an optimization, not a prerequisite. Actual topology and placement depend on the host, CUDA runtime, driver, device visibility, and workload. Do not treat a benchmark topology or bandwidth figure as a property of every deployment.

## 5. Execution backends

### CPU

The CPU path is the deterministic baseline and the fallback path. The default compute example consumes a packed array of IEEE-754 binary64 values encoded big-endian and returns the same representation after the reference operation. Other request kinds have their own payload contracts; see the protocol guide.

### GPU

The optional CUDA-aware path detects GPU topology and uses the ADI compute entry point plus `Placer`. Configure with `-DENABLE_CUDA=ON`. CMake probes for a CUDA compiler and toolkit; if unavailable it reports a CPU-only build rather than silently claiming GPU support. TensorRT is a separate optional runtime target and requires CUDA plus user-provided TensorRT libraries.

### NPU

NPU execution is opt-in and requires a concrete executor:

- `--npu-simulated` installs a CPU-emulated executor for conformance and CI. It is not hardware acceleration and must not be used as evidence of NPU performance.
- The optional `ENABLE_ETHOSU_DRM=ON` build compiles the Linux mainline DRM Ethos-U executor. At runtime, `--npu-ethosu-bundle FILE` supplies a Vela-generated `YU85BND1` bundle. An optional `--ethosu-device /dev/dri/renderD*` pins the device; otherwise discovery uses `ETHOSU_DRM_DEVICE` and then writable render nodes.
- The real executor requires a compatible kernel exposing the Ethos-U DRM UAPI, a supported device, permissions to open its render node, and a bundle compiled for that target. Build success and simulator tests do not establish physical-board qualification.
- `--npu-enabled` only enables the service-side gate; by itself it does not create an executor. Prefer the explicit simulator or bundle options when configuring an NPU path.

See [ETHOSU_U85_BUNDLE.md](ETHOSU_U85_BUNDLE.md) for the bundle contract.

## 6. Dedicated payload handlers

The broker routes certain `kind` values before the default numeric compute path:

- `geospatial.frame.v1`: validates an NXR1 frame, including finite numeric values and bounded payload length, then echoes the validated frame.
- `media.stream.v1`: validates a MED1 video/audio/generic-data chunk and echoes an acknowledgement payload. This is transport validation, not media transcoding or inference.
- WASM task and lifecycle kinds are handled by the WASM sandbox integration compiled into `broker_service`.

These paths are separate contracts. A client must not send an NXR1 or MED1 payload as if it were a packed-double inference payload. See [WASM_INTEROP.md](WASM_INTEROP.md) and the relevant protocol sections.

### WASM task and lifecycle boundary

The WASM integration is a broker protocol and version-lifecycle manager, **not a Wasm runtime**. YuKKi-OS owns bytecode validation, Wasmtime compilation, guest instantiation, linear-memory limits and host imports. Overhauled does not receive or compile a guest module during `Prepare`.

The C++ `WasmSandboxManager::prepare_module` records the sandbox/module/version identity, state, lease and optional GPU assignment. It does not parse `.wasm` bytes or retain a compiled artifact. `handle_task` checks the registered version and in-flight state, then calls an application-supplied `ComputeFn` when provided; otherwise it uses the broker's reference numeric compute path for packed binary64 payloads or echoes other payloads. The task payload is work input, not executable Wasm code.

The manager tracks active tasks, rejects work for draining/stopped versions, and refuses release while tasks remain in flight. Actual guest compilation, linking, instantiation and any code composition remain on the YuKKi-OS host. WSM1 lifecycle operations do not merge multiple Wasm modules or policy functions into a compiled artifact.

TensorRT is a separate optional `gpu_runtime_server` target; it is not an automatic execution engine behind every WSM1 task. The Ethos-U85 path is also opt-in and accepts its documented Vela bundle contract. See [WASM_INTEROP.md](WASM_INTEROP.md) for the wire boundary and [SYSADMIN_HOWTO.md](SYSADMIN_HOWTO.md) for deployment configuration.

## 7. Concurrency, limits, and lifecycle

The server uses a bounded worker pool, per-client sessions, a bounded in-flight request count, a global queued-task ceiling, a default maximum of 128 connected clients, a default per-connection in-flight cap of 32, and a default socket timeout of 5 seconds. Worker count is auto-selected and capped at 256 unless configured through the C++ API.

The listener binds to all interfaces (`0.0.0.0`). Restrict ingress with firewall rules or a private network. Shutdown closes the listener and active client sockets and drains/stops worker activity according to the server lifecycle.

The broker has stdout/stderr startup and shutdown diagnostics. It does not currently expose a built-in HTTP health endpoint or metrics endpoint; monitoring must be supplied by the process supervisor, host telemetry, or the authenticated proxy layer.

## 8. Build-time feature matrix

| CMake option | Default | Effect |
|---|---:|---|
| `BUILD_BROKER` | ON | Builds broker service and `broker_server` |
| `ENABLE_CUDA` | ON | Probes CUDA compiler/toolkit; falls back to CPU-only if unavailable |
| `ENABLE_TENSORRT` | OFF | Builds `gpu_runtime_server`; requires CUDA and TensorRT |
| `ENABLE_ETHOSU_DRM` | OFF | Compiles the Linux Ethos-U DRM executor; Linux only |
| `ENABLE_INTEROP_ENCRYPTION` | OFF | Requests an optional source file that must exist in the checkout; verify availability before enabling |
| `BUILD_EXAMPLES` | ON | Builds example binaries |

CPU-only broker builds should explicitly set `-DENABLE_CUDA=OFF -DENABLE_TENSORRT=OFF -DBUILD_BROKER=ON`. Optional hardware backends should be enabled only after their toolchains, runtime libraries, kernel interfaces, and devices are installed.

## 9. Heuristic tuning and validation

Tune one decision boundary at a time, using the benchmark harness and representative payloads:

1. Establish a CPU-only correctness baseline.
2. Confirm backend selection from the response, not merely from the requested preference.
3. Compare GPU placement with the same workload, hardware, concurrency, and warm-up policy.
4. Sweep the GPU byte threshold and backlog threshold; record throughput, p50/p95/p99 latency, queue depth, GPU utilization, and transfer topology.
5. Measure wall power if making energy claims. Throughput-only results cannot establish power or OPEX savings.
6. Exercise accelerator failure with both fallback-safe and fallback-unsafe profiles.
7. Keep protocol conformance and adversarial tests enabled while tuning.

The benchmark values in [BENCHMARK_RESULTS.md](BENCHMARK_RESULTS.md) are workload-specific observations, not universal guarantees. Do not copy a threshold from one GPU topology or dataset into production without rerunning the workload.

## Heuristic values and interpretation

The dispatcher uses explicit task metadata rather than treating a requested
device as proof of hardware execution:

- Forced CPU ("--cpu-only" or BRK2 device_preference=cpu) wins first.
- Exact-precision, verification-sensitive and cryptographic profiles remain
  on CPU unless the profile explicitly marks an accelerator path verified.
- NPU is selected for inference profiles only when the NPU gate is enabled and
  the configured executor reports available. --npu-enabled alone is only
  the gate; --npu-simulated installs CPU emulation, while
  ENABLE_ETHOSU_DRM=ON plus a compatible YU85BND1 bundle enables the
  optional physical Linux path.
- GPU is selected for data-parallel profiles when the GPU is available and
  the configured byte threshold (default 1 MiB) or non-zero FLOP threshold is
  met. A zero FLOP threshold disables that criterion.
- Without a task profile, only an operator-configured GPU default can
  auto-offload; a profile-less --default-backend npu request still resolves
  to CPU. A BRK2 device_preference=npu supplies an inference profile, but
  the response's selected backend remains authoritative.

The dispatcher and service implementation have different sources of profile
metadata: in-process callers supply TaskProfile, while the BRK2 adapter maps
wire-level device_preference to a basic profile when no in-process profile
exists. That wire preference path marks the generic numeric request as
fallback-safe; application-specific, side-effecting operations should not be
declared fallback-safe unless CPU re-execution is actually idempotent.

GPU placement is a second stage after GPU backend selection. The current
topology detector converts CUDA peer performance-rank/native-atomic signals
into relative link hints (100/90 for preferred peer links and 10 for PCIe).
Those values are ordinal heuristic scores, not measured GB/s, latency, or
a portable physical bandwidth guarantee. Re-profile on each target system.

For the full operational sequence, use [System administration](SYSADMIN_HOWTO.md).

## 10. Source-of-truth implementation map

| Concern | Implementation |
|---|---|
| Build features and target graph | `CMakeLists.txt` |
| BRK1/BRK2 framing and schemas | `src/broker_protocol.cpp`, `src/brk2_protocol.cpp`, corresponding headers |
| TCP listener, sessions, limits | `src/broker_server.cpp`, `include/broker_server.h` |
| CLI and runtime backend wiring | `src/broker_server_main.cpp` |
| Backend policy and NPU interface | `include/backend_dispatch.h`, `include/npu_device.h` |
| Broker routing and execution | `src/broker_service.cpp`, `include/broker_service.h` |
| GPU topology and placement | `src/nvlink_placement.cpp`, `include/nvlink_placement.h` |
| Ethos-U DRM execution | `src/ethosu_u85_drm_executor.cpp`, `include/ethosu_u85_drm_executor.h` |
| NXR1 and MED1 handlers | `src/geospatial_frame.cpp`, `src/media_stream.cpp` |
| WASM sandbox | `src/wasm_sandbox.cpp`, `include/wasm_sandbox.h` |

## YuKKi-directed worker fabric

Overhauled now has class-isolated, dynamically growing FIFO worker queues for Control, RealtimeSpatial, Throughput, ExactCpu, and Inference work. YuKKi-OS supplies optional BRK2 worker intent; Overhauled performs broker admission, Linux NUMA CPU affinity, accelerator dispatch, and telemetry. The queues do not have fixed per-class entry limits. A process-wide outstanding-request and in-flight-memory budget protects the broker from memory exhaustion, and reservations remain held after client disconnect until task completion. FIFO ordering is per class; isolated class pools intentionally do not provide a single global order. See [YUKKI_WORKER_FABRIC.md](YUKKI_WORKER_FABRIC.md) for the BRK2 field contract, ownership rules, capacity controls, NUMA behavior, and operational validation.
