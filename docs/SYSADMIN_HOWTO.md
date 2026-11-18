# User and Sysadmin How-To: Configure, Build, Run, and Verify Overhauled

This guide walks through a clean setup of the Overhauled broker and its optional execution backends. Commands assume a Linux host and a shell. Replace example paths, addresses, ports, and service names with values from your environment.

Overhauled is the execution/placement plane. YuKKi-OS or another control plane remains a separate application and connects over the broker's binary TCP protocol. The broker itself does not provide TLS, authentication, HTTP endpoints, or a web dashboard.

## 1. Choose the configuration you need

Start with the smallest working configuration:

| Profile | Build options | Runtime | Intended use |
|---|---|---|---|
| CPU-only broker | `ENABLE_CUDA=OFF`, `ENABLE_TENSORRT=OFF`, `BUILD_BROKER=ON` | `--cpu-only` | First install, CI, non-GPU hosts |
| CUDA-aware broker | `ENABLE_CUDA=ON`, `ENABLE_TENSORRT=OFF` | default or `--default-backend gpu` | GPU topology and eligible GPU workloads |
| TensorRT runtime | CUDA ON, TensorRT ON, valid `TENSORRT_ROOT` | `gpu_runtime_server` | Separate TensorRT runtime target |
| NPU simulator | broker build | `--npu-simulated` | CI/conformance only; CPU emulation |
| Ethos-U85 hardware | Linux, `ENABLE_ETHOSU_DRM=ON` | `--npu-ethosu-bundle FILE` | Compatible physical Ethos-U device and Vela bundle |

Do not begin with optional accelerators. Prove the CPU-only path first, then enable one backend at a time.

### WASM task integration is host-side

Overhauled does not build or run the Wasm guest. Do not upload a `.wasm` file to the broker expecting it to compile or execute it. Build and validate the guest in YuKKi-OS, instantiate it with the YuKKi-OS Wasmtime host, and send only the host-mediated task input and lifecycle messages to this broker.

For YuKKi-OS, the WSM1 adapter is opt-in and configured in the **YuKKi-OS process**, not here: set `YUKKI_GPU_ADAPTER_ENABLED=true`, set `YUKKI_GPU_USE_WSM1=true` when using the WSM1 task/lifecycle wire mode, and point `YUKKI_GPU_BROKER_ENDPOINT` at this broker. See [YuKKi-OS SYSADMIN_HOWTO](https://github.com/rakshas-oss/YuKKi-OS/blob/main/docs/SYSADMIN_HOWTO.md) for guest compilation, Wasmtime limits, host authentication, and module version lifecycle.

The broker's `Prepare` operation registers a module/version identity and placement lease; it does not receive the executable artifact or compile it. The broker's `TaskRequest` carries application task data and is dispatched to the configured compute callback or broker reference path. Wasm bytecode compilation, multi-module linking/merge compilation, guest-memory isolation and guest instantiation stay in YuKKi-OS.

## 2. Install prerequisites

Required for the broker:

- Linux (the broker uses POSIX sockets and pthreads)
- CMake 3.18 or newer
- A C++17-capable compiler and standard library
- Git and a build tool supported by CMake (for example, Ninja or Make)

Optional:

- CUDA compiler/toolkit and NVIDIA driver for GPU topology and CUDA-aware placement
- TensorRT headers and libraries for the separate TensorRT target
- Linux kernel and device exposing the mainline Ethos-U DRM UAPI for physical NPU execution
- A Vela-generated `YU85BND1` bundle compatible with the attached Ethos-U target

Check the basic tools:

```bash
cmake --version
c++ --version
git --version
```

For CUDA deployments, also verify the driver/toolkit with the vendor tools installed on that host (commonly `nvidia-smi` and `nvcc --version`). CMake's successful CUDA detection does not replace checking that the runtime driver can see the intended GPUs.

## 3. Get the source and select a version

```bash
git clone https://github.com/rakshas-oss/overhauled.git
cd overhauled
git fetch --tags origin
```

For a release deployment, check out the approved release tag after it exists:

```bash
git checkout v1.1.4
```

If the tag has not yet been created, use the reviewed release commit or the intended branch supplied by your release process. Do not assume an open pull request is already part of `main` or a published release.

## 4. Configure a CPU-only broker build (recommended first run)

From the repository root:

```bash
cmake -S . -B build-cpu \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_BROKER=ON \
  -DBUILD_EXAMPLES=OFF \
  -DENABLE_CUDA=OFF \
  -DENABLE_TENSORRT=OFF
```

Review CMake's summary and ensure the broker target is configured. Then build:

```bash
cmake --build build-cpu --parallel
```

Run the complete configured test suite:

```bash
ctest --test-dir build-cpu --output-on-failure
```

Expected result: the build completes and all registered tests pass. If a test fails, stop here and diagnose it before enabling hardware-specific options.

## 5. Verify the command-line interface

```bash
./build-cpu/broker_server --help
```

The broker CLI supports:

- `[port]` (default `9090`)
- `--cpu-only`
- `--default-backend cpu|gpu|npu`
- `--gpu-offload-threshold BYTES`
- `--npu-enabled`
- `--npu-simulated`
- `--npu-ethosu-bundle FILE`
- `--ethosu-device /dev/dri/renderD*`

Important: `--npu-enabled` only enables the service-side NPU gate. It does not create an NPU executor. Use `--npu-simulated` for CPU-emulated tests or `--npu-ethosu-bundle` with an actual supported device. The simulated executor is not hardware acceleration. Although `--default-backend npu` is accepted by the CLI, it does not auto-route profile-less requests to NPU under the current dispatcher policy. For a wire client, use BRK2 `device_preference=npu` and configure a concrete executor.

## 6. Start the CPU-only broker and check the listener

Run in the foreground for the first verification:

```bash
./build-cpu/broker_server 9090 --cpu-only
```

In a second terminal:

```bash
ss -ltnp | grep ':9090'
```

The server binds to all interfaces (`0.0.0.0`), not just loopback. Do not expose this listener directly to an untrusted network. Stop the foreground server with Ctrl-C after verification.

There is no built-in HTTP health route or metrics endpoint. A successful TCP listener check confirms the socket is listening; it does not prove end-to-end YuKKi-OS interoperability.

## 7. Optional: configure CUDA-aware GPU placement

Install and verify a compatible NVIDIA driver and CUDA toolkit first. Configure a separate build directory so the CPU-only build remains available for rollback:

```bash
cmake -S . -B build-cuda \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_BROKER=ON \
  -DENABLE_CUDA=ON \
  -DENABLE_TENSORRT=OFF
cmake --build build-cuda --parallel
ctest --test-dir build-cuda --output-on-failure
```

CMake probes for a CUDA compiler and toolkit. If it cannot find a usable CUDA toolchain, it reports that it is building CPU-only; read the configure output rather than assuming GPU support is active.

Start with CPU-safe default dispatch:

```bash
./build-cuda/broker_server 9090
```

GPU selection requires eligible request metadata and an available GPU. For workloads without profile metadata, an operator may explicitly choose a GPU default:

```bash
./build-cuda/broker_server 9090 --default-backend gpu
```

The `--gpu-offload-threshold BYTES` option changes the byte threshold for data-parallel work. It is not a guarantee that every request will use a GPU. Confirm the actual response backend and selected GPU. CPU results normally report `selected_gpu = -1`.

For topology diagnostics on the host, use the NVIDIA tooling appropriate to the installed driver (for example, `nvidia-smi -L` and `nvidia-smi topo -m`). NVLink is optional; PCIe-only systems can still use the available GPU placement path.

## 8. Optional: configure the NPU simulator for conformance

The simulator is CPU emulation and must not be described as an NPU performance test. It is useful to exercise backend-selection and protocol paths without physical hardware.

```bash
./build-cpu/broker_server 9090 --npu-simulated
```

A BRK2 client can request `device_preference=npu`; verify the response's reported backend rather than inferring execution from the request. The simulator's numeric behavior is a test implementation and is not a model-specific inference result.

## 9. Optional: build and run the physical Ethos-U85 backend

Only use this section on Linux with a kernel exposing the mainline Ethos-U DRM UAPI, a supported device, access permissions for its render node, and a Vela-generated `YU85BND1` bundle compatible with that device.

Build in a separate directory:

```bash
cmake -S . -B build-ethosu \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_BROKER=ON \
  -DENABLE_CUDA=OFF \
  -DENABLE_TENSORRT=OFF \
  -DENABLE_ETHOSU_DRM=ON
cmake --build build-ethosu --parallel
ctest --test-dir build-ethosu --output-on-failure
```

Check that the intended render node exists and that the service account can open it. Then start with the model bundle:

```bash
./build-ethosu/broker_server 9090 \
  --npu-ethosu-bundle /path/to/model.yu85
```

To pin a particular node:

```bash
./build-ethosu/broker_server 9090 \
  --npu-ethosu-bundle /path/to/model.yu85 \
  --ethosu-device /dev/dri/renderD128
```

Alternatively, set `ETHOSU_DRM_DEVICE` for device discovery. The bundle layout, quantization rules, and device requirements are documented in [ETHOSU_U85_BUNDLE.md](ETHOSU_U85_BUNDLE.md).

A successful compile or parser test is not proof of physical inference. Hardware qualification requires a successful inference on the target board using a bundle compiled for that device, with correct input/output tensor dimensions and quantization.

## 10. Optional: build the TensorRT runtime target

TensorRT is separate from the broker's basic GPU placement path. It requires CUDA plus user-installed TensorRT headers and libraries:

```bash
cmake -S . -B build-tensorrt \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_CUDA=ON \
  -DENABLE_TENSORRT=ON \
  -DTENSORRT_ROOT=/path/to/tensorrt
cmake --build build-tensorrt --parallel
```

CMake fails configuration if TensorRT was requested but the required headers or libraries cannot be found. When built, the separate executable is `gpu_runtime_server`; do not assume it is the same listener or protocol as `broker_server`.

## 11. Connect YuKKi-OS or another client

Configure the control plane to connect to the broker's host and TCP port through the authenticated proxy or private network boundary.

Protocol requirements:

1. Use the YuKKi-OS BRK1/BRK2 binary adapter. The generic JSON `BrokerTask` / `BrokerResult` transport is not wire-compatible with this server.
2. Send a four-byte big-endian body length followed by the BRK1 or BRK2 frame body.
3. Keep required identifiers (`task_id`, `source`, `destination`, `kind`) non-empty and `timeout_ms` within the protocol range of 1..300000 ms.
4. Use a stable `source` value when sticky client-to-GPU affinity is desired.
5. For BRK2, use `device_preference` to request `cpu`, `gpu`, or `npu`; the response indicates the backend actually selected. BRK1 carries no such backend preference field.
6. Keep frame, payload, and string sizes within the documented protocol limits.
7. Treat `selected_gpu = -1` as CPU execution, not as proof of failure.

The broker does not negotiate protocol versions or accept JSON HELLO messages. A TCP proxy must forward the raw byte stream unchanged; do not insert HTTP framing.

See [YUKKI_INTEROP.md](YUKKI_INTEROP.md), [BROKER_PROTOCOL.md](BROKER_PROTOCOL.md), and [BRK_CONFORMANCE_V1.md](BRK_CONFORMANCE_V1.md).

## 12. Secure the network boundary

The broker listens on all interfaces and does not provide built-in TLS or authentication. Before production use:

1. Place the broker on a private subnet or bind access through a host/network firewall.
2. Permit inbound TCP only from the authenticated gateway, service mesh, or proxy.
3. Terminate mTLS/client identity at that gateway.
4. Forward the raw BRK1/BRK2 TCP stream unchanged to the broker.
5. Do not expose port 9090 publicly.

Illustrative UFW rules (replace the example source address with the real gateway address):

```bash
sudo ufw allow from 192.0.2.10 to any port 9090 proto tcp
sudo ufw deny 9090/tcp
```

Check your firewall's rule ordering and existing policy before applying rules on a remote host.

## 13. Run as a service (example only)

The broker has no built-in config file. Runtime settings are command-line arguments and process environment, plus the external proxy/firewall configuration.

Create a restricted service account and directories if your deployment policy calls for them:

```bash
sudo useradd --system --home /var/lib/overhauled --shell /usr/sbin/nologin overhauled
sudo install -d -o overhauled -g overhauled /var/lib/overhauled
sudo install -d -o overhauled -g overhauled /var/log/overhauled
```

For a simple foreground-managed process, use your organization's service supervisor (systemd, container runtime, or equivalent) with:

- executable: absolute path to `broker_server`
- arguments: selected port and backend flags
- user/group: restricted service identity
- restart policy: defined by local operations policy
- stdout/stderr: captured by the supervisor
- network policy: only authenticated gateway sources allowed

Do not copy a generic unit file without adapting binary path, user/group, environment, device permissions, and hardening directives to the host.

## 14. Verify the decision heuristics before tuning

Complete the CPU-only test first. Then enable one accelerator at a time and
verify the response backend instead of inferring it from command-line flags.

1. Record the build configuration, available device list, CMake summary,
   broker version, and test results.
2. Run CPU-only and confirm responses report backend cpu and selected_gpu=-1.
3. For CUDA, use a separate ENABLE_CUDA=ON build and compare identical
   workloads. The default data-parallel byte threshold is 1 MiB; the
   in-process FLOP threshold is disabled at zero unless explicitly set.
   The default placement backlog threshold is 32.
4. For NPU conformance, launch --npu-simulated and send BRK2
   device_preference=npu. Treat this as CPU emulation only. For hardware,
   use the opt-in Linux Ethos-U DRM build and a compatible Vela bundle.
5. Compare the returned backend and selected device with p50/p95/p99 latency,
   throughput, queue depth, in-flight count, and wall power on the same
   hardware and dataset. Tune one threshold at a time and keep BRK conformance
   tests enabled.
6. Exercise both fallback-safe and fallback-unsafe failure cases. Never mark a
   non-idempotent or side-effecting request fallback-safe solely to obtain a
   CPU retry.

A preference is a request, not an execution guarantee. In particular,
--default-backend npu is accepted by the CLI but does not auto-route
profile-less requests to NPU. Use a BRK2 device_preference=npu or an
in-process inference profile and ensure a concrete NPU executor is enabled.

## 15. Verify a deployment

Run the checks in order:

1. **Build and unit/integration tests:** `ctest --test-dir BUILD_DIR --output-on-failure`.
2. **CLI:** `BUILD_DIR/broker_server --help`.
3. **Listener:** `ss -ltnp | grep ':9090'` (replace the port if changed).
4. **Logs:** check supervisor stdout/stderr for startup errors and clean shutdown.
5. **Protocol:** run the repository's BRK1/BRK2 conformance tests and a real client-to-broker integration test.
6. **Backend:** send a request eligible for the chosen backend and inspect the response backend and selected GPU. Do not infer accelerator use from build flags alone.
7. **Security:** confirm a host outside the permitted gateway path cannot connect.

Useful targeted tests from the build directory:

```bash
ctest --test-dir BUILD_DIR --output-on-failure \
  -R 'brk2_protocol_test|brk_conformance_test|broker_protocol_test|broker_service_test|backend_dispatch_test|broker_server_e2e_test'
```

There is no built-in HTTP health endpoint or metrics endpoint. Add host-level monitoring or proxy-level health checks appropriate to your environment; do not configure probes against undocumented `/health` or `/metrics` routes.

## 16. Upgrade and rollback

1. Keep the current known-good binary and build directory.
2. Build the new release in a separate directory.
3. Run the full test suite and protocol conformance checks.
4. Verify the new binary's CLI and backend response behavior in staging.
5. Switch the supervisor to the new binary and restart during the approved window.
6. Confirm listener, logs, protocol integration, and backend selection.
7. Roll back by stopping the new process and restoring the previous binary/build path if verification fails.

Coordinate any BRK wire-format or payload-contract change with the control-plane client before rollout.

## 17. Troubleshooting

### CMake configures CPU-only despite ENABLE_CUDA=ON

Read the CMake configure output. Confirm the CUDA compiler and toolkit are installed and discoverable. If they are not, use the CPU build or correct the toolchain path before expecting GPU selection.

### Broker fails to start

Check port range (1..65535), whether the port is already in use, service-account permissions, and supervisor logs. Confirm that `BUILD_BROKER=ON` was used.

### BRK requests are rejected

Check the four-byte big-endian frame prefix, BRK1/BRK2 magic/version, required non-empty identifiers, timeout range, and configured frame/payload/string limits. Do not send JSON request bodies to the binary broker.

### Response reports CPU / selected_gpu = -1

This is expected with `--cpu-only`, no usable GPU topology, a CPU preference, a request without GPU-eligible metadata, or an unavailable accelerator. Confirm the response backend and host device visibility before changing thresholds.

### NPU preference falls back to CPU

Confirm the broker was started with `--npu-simulated` for conformance or with an Ethos-U bundle in a build configured with `ENABLE_ETHOSU_DRM=ON`. `--npu-enabled` alone does not instantiate an executor. For physical hardware, check the kernel driver, render-node permissions, bundle compatibility, and device selection.

### Client connects but times out or disconnects

Check proxy TCP pass-through, the default 5-second socket timeout, client in-flight cap (32), maximum concurrent clients (128), and whether the client is sending complete frames. Increasing a client timeout does not preempt work already executing on the broker.
