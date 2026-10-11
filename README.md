# Overhauled v1.0

A production-ready C++ library for NVLink-aware GPU task placement, topology-aware scheduling, and broker interoperability.

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)](#)
[![Release](https://img.shields.io/badge/Release-v1.0-blueviolet)](#)
[![Broker](https://img.shields.io/badge/Protocol-BRK1%2Fv1-orange)](#)

## Overview

Overhauled helps GPU systems place work on the right device with minimal host staging and better tail latency. The library combines:

- NVLink-aware topology detection and peer selection
- Sticky home-GPU affinity for reuse-friendly scheduling
- Intelligent fallback from NVLink -> PCIe -> least-loaded GPU
- Backend dispatch for CPU / GPU / NPU execution decisions
- A broker protocol for interoperable request/response service flows

This repository is the placement and execution plane. It does not replace the control plane; instead it exposes a stable, versioned protocol boundary for systems like `rakshas-oss/YuKKi-OS` and other authenticated service meshes.

## What's new in v1.0

Version 1.0 marks the first stable release of the project and includes:

- a stable broker protocol contract (`BRK1` / version 1)
- production-oriented placement and fallback behavior
- broader documentation covering operations, deployment, benchmarking, and protocol semantics
- CPU-only build and test support for CI and non-GPU environments
- clearer packaging and distribution guidance for Conan, vcpkg, and Homebrew

## Quick start

### Install from source

```bash
git clone https://github.com/rakshas-oss/overhauled.git
cd overhauled
mkdir -p build && cd build
cmake -S .. -B . \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_CUDA=OFF \
  -DENABLE_TENSORRT=OFF \
  -DBUILD_BROKER=ON
cmake --build . -j"$(nproc)"
```

### Minimal library example

```cpp
#include "nvlink_placement.h"
#include <iostream>

using namespace nvlink;

int main() {
    auto topo = GpuTopology::detect();
    topo.enable_peer_access();

    Placer placer(topo, 32);

    int home_gpu = placer.assign_home(42);
    int target_gpu = placer.place(42, [&](int gpu) {
        return gpu_queue_depth[gpu];
    });

    enqueue_task_on_gpu(target_gpu);
    return 0;
}
```

### Run the broker service in CPU-only mode

```bash
cd build
./broker_server 9090 --cpu-only
```

This is useful for CI, non-GPU hosts, and smoke tests while preserving the same protocol contract.

## Documentation

- [docs/RELEASE_NOTES.md](docs/RELEASE_NOTES.md) — v1.0 release summary and migration notes
- [docs/LIBRARY.md](docs/LIBRARY.md) — API usage and integration guide
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — design and topology view
- [docs/BROKER_PROTOCOL.md](docs/BROKER_PROTOCOL.md) — BRK1 broker protocol contract
- [docs/WASM_INTEROP.md](docs/WASM_INTEROP.md) — WASM interoperability and sandbox boundary notes
- [docs/SYSADMIN_HOWTO.md](docs/SYSADMIN_HOWTO.md) — Linux deployment and operations guide
- [docs/BENCHMARK_RESULTS.md](docs/BENCHMARK_RESULTS.md) — performance benchmarks and latency data
- [docs/PUBLISHING.md](docs/PUBLISHING.md) — packaging and publishing guide
- [docs/WHITEPAPER.md](docs/WHITEPAPER.md) — design rationale and background
- [DEPLOYMENT_GUIDE.md](DEPLOYMENT_GUIDE.md) — production deployment guidance
- [OPERATIONS_RUNBOOK.md](OPERATIONS_RUNBOOK.md) — operational playbook

## Build modes

```bash
# CPU-first / CI friendly
cmake -S . -B build -DENABLE_CUDA=OFF -DENABLE_TENSORRT=OFF

# Auto-detect CUDA runtime and topology support
cmake -S . -B build -DENABLE_CUDA=ON -DENABLE_TENSORRT=OFF

# TensorRT runtime server
cmake -S . -B build -DENABLE_CUDA=ON -DENABLE_TENSORRT=ON -DTENSORRT_ROOT=/path/to/tensorrt
```

## Key features

- Sticky affinity and home-GPU assignment
- NVLink-aware rebalancing before PCIe fallback
- Thread-safe placement decisions for concurrent task dispatch
- Broker protocol support for external control-plane communication
- CPU-only compatibility for smoke tests and development

## Security and operating model

Overhauled is designed as the execution plane. The broker transport is bounded and versioned, but it is not an authentication layer by itself. Production deployments should place the service behind an authenticated proxy, service mesh, or mTLS boundary and apply network restrictions as appropriate.

## Performance

- Lower latency than blind round-robin placement
- Better tail-latency behavior under multi-client load
- High GPU utilization when topology-aware placement is enabled

## License

MIT License — see [LICENSE](LICENSE).

## References

- [NVIDIA CUDA P2P Documentation](https://docs.nvidia.com/cuda/cuda-runtime-api/)
- [NVLink Architecture](https://www.nvidia.com/en-us/data-center/nvlink/)
- [YuKKi-OS](https://github.com/rakshas-oss/YuKKi-OS)
