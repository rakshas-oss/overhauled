# Overhauled v1.1.1

Overhauled is a C++17 placement and execution library with an interoperable broker service. It provides topology-aware GPU scheduling and broker dispatch for CPU, GPU, and optional NPU execution. It is the execution plane, not a replacement for a control plane such as [YuKKi-OS](https://github.com/rakshas-oss/YuKKi-OS).

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)](#)
[![Release](https://img.shields.io/badge/Release-v1.1.1-blueviolet)](#)
[![Broker](https://img.shields.io/badge/Protocol-BRK1%2FBRK2-orange)](#)

## Capabilities

- NVLink-aware topology detection, sticky home-GPU affinity, and queue-aware placement.
- Broker dispatch for CPU, GPU, and NPU preferences, with the selected backend returned to clients.
- BRK1/v1 compatibility and BRK2/v2 TLV framing on the same TCP listener.
- A shared Rust/C++ protocol conformance corpus and CPU-only test suite.
- Opt-in mainline Linux DRM Ethos-U85 executor, with a documented Vela-generated `YU85BND1` bundle format.
- Optional CUDA/TensorRT GPU runtime features; deterministic CPU-only broker operation remains available.

## Control-plane interoperability

YuKKi-OS remains the control plane; Overhauled owns broker execution and accelerator placement. YuKKi-OS uses the BRK1 or BRK2 binary broker envelope for the Overhauled endpoint. Its separate generic JSON broker client is not compatible with this listener. BRK2 request field `device_preference` (field 10) can request `cpu`, `gpu`, or `npu`; response field `backend` (field 7) reports which backend actually handled the request. A preference is not a hardware guarantee. See [docs/YUKKI_INTEROP.md](docs/YUKKI_INTEROP.md) for the cross-repository boundary and hardware qualifications.

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

This mode is useful for CI, development, and non-GPU hosts. The service uses bounded broker framing, but the protocol is not an authentication layer; deploy it only on a trusted network or behind an independently secured proxy.

## Optional Ethos-U85 Linux DRM backend

Physical NPU execution is opt-in and requires Linux with a kernel exposing the mainline Ethos-U DRM UAPI, plus a compatible Vela-generated bundle. Configure and build with:

```bash
cmake -S . -B build \
  -DENABLE_ETHOSU_DRM=ON \
  -DENABLE_CUDA=OFF \
  -DENABLE_TENSORRT=OFF \
  -DBUILD_BROKER=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Then launch using a `YU85BND1` model bundle:

```bash
./build/broker_server 9090 --npu-ethosu-bundle model.yu85
# Optional explicit DRM render node:
./build/broker_server 9090 --npu-ethosu-bundle model.yu85 --ethosu-device /dev/dri/renderD128
```

`--npu-simulated` is CPU emulation for CI/conformance, not hardware acceleration. A successful build, parser test, or simulator run does not establish physical-board inference. See [docs/ETHOSU_U85_BUNDLE.md](docs/ETHOSU_U85_BUNDLE.md) for bundle layout and validation limits.

## Other build modes

```bash
# CUDA-aware placement, without TensorRT
cmake -S . -B build -DENABLE_CUDA=ON -DENABLE_TENSORRT=OFF

# TensorRT runtime server (requires CUDA and user-provided TensorRT)
cmake -S . -B build -DENABLE_CUDA=ON -DENABLE_TENSORRT=ON -DTENSORRT_ROOT=/path/to/tensorrt
```

## Documentation

- [Broker protocol](docs/BROKER_PROTOCOL.md) · [BRK2 conformance](docs/BRK2_CONFORMANCE_V1.md) · [Executable BRK corpus](docs/BRK_CONFORMANCE_V1.md)
- [YuKKi-OS interoperability](docs/YUKKI_INTEROP.md) · [WASM interoperability](docs/WASM_INTEROP.md)
- [Ethos-U85 bundle and hardware boundary](docs/ETHOSU_U85_BUNDLE.md)
- [Architecture](docs/ARCHITECTURE.md) · [Library guide](docs/LIBRARY.md)
- [System administration](docs/SYSADMIN_HOWTO.md) · [Operations runbook](OPERATIONS_RUNBOOK.md) · [Deployment guide](DEPLOYMENT_GUIDE.md)
- [Publishing](docs/PUBLISHING.md) · [Release notes](docs/RELEASE_NOTES.md) · [Benchmarks](docs/BENCHMARK_RESULTS.md)

## License

MIT License — see [LICENSE](LICENSE). Optional CUDA Toolkit and TensorRT components are user-supplied proprietary NVIDIA software, not redistributed under this project's license; see [NOTICE](NOTICE).
