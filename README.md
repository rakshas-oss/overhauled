# NVLink-Aware GPU Task Placement Library

A production-ready C++ library for intelligent GPU scheduling using NVLink topology awareness.

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)](#)

## 🎯 Overview

This library provides **NVLink-aware task placement** for GPU task schedulers. Instead of blind round-robin assignment, it:

1. **Detects GPU topology** - Queries NVIDIA driver to identify NVLink, PCIe, and peer access links
2. **Maintains sticky affinity** - Each client pinned to a home GPU for memory reuse
3. **Rebalances intelligently** - Offloads backed-up work to NVLink peers (600+ GB/s) instead of PCIe neighbors (16 GB/s), but gracefully falls back to PCIe on older pre-NVLink devices.
4. **Scales efficiently** - Thread-safe, zero external dependencies, production-proven
5. **Serves ADI binary protocol traffic** - Optional multi-threaded server built on `nvlink::Placer`

## 🚀 Quick Start

### Installation

```bash
git clone https://github.com/rakshas-oss/overhauled.git
cd overhauled
mkdir build && cd build
cmake ..
make
sudo make install
```

### Minimal Example

```cpp
#include "nvlink_placement.h"
#include <iostream>

using namespace nvlink;

int main() {
    auto topo = GpuTopology::detect();
    topo.print_info();
    topo.enable_peer_access();

    Placer placer(topo, 32);

    int home_gpu = placer.assign_home(client_socket);
    int target_gpu = placer.place(client_socket, [&](int gpu) {
        return gpu_queues[gpu].size();
    });

    gpu_queues[target_gpu].enqueue(task);
    return 0;
}
```

### ADI Server Example

```bash
cmake -S . -B build -DBUILD_EXAMPLES=ON
cmake --build build
./build/example_adi_server 8080 32
```

The ADI server accepts a 4-byte big-endian length prefix followed by a 40-byte payload containing 5 big-endian doubles. It replies with a 13-byte header (`timestamp_ns`, `sequence_id`, `packet_type`) plus a 40-byte payload. The first request receives a Primary packet; later requests receive both Primary and Delta packets sharing the same `sequence_id`, where Delta is `current_result - previous_result`.

## 📊 Key Features

- **Sticky Affinity**: Each client pinned to home GPU
- **NVLink-Aware Rebalancing**: Prefers fast NVLink links over PCIe (gracefully falls back to PCIe on pre-NVLink devices)
- **Thread-Safe**: Safe for concurrent task dispatch
- **Customizable Compute**: Supply your own GPU callback or use the default scaling implementation

## 📚 Documentation

- [RELEASE NOTES](docs/RELEASE_NOTES.md) - What changed in recent releases
- [LIBRARY.md](docs/LIBRARY.md) - API reference and integration guide
- [ARCHITECTURE.md](docs/ARCHITECTURE.md) - System design
- [BROKER_PROTOCOL.md](docs/BROKER_PROTOCOL.md) - Broker interoperability contract and protocol v1
- [BENCHMARK_RESULTS.md](docs/BENCHMARK_RESULTS.md) - Performance benchmarking
- [PUBLISHING.md](docs/PUBLISHING.md) - Packaging and publishing instructions
- [WHITEPAPER.md](docs/WHITEPAPER.md) - Design rationale and background

## 🔧 Build

```bash
mkdir build && cd build
cmake -DENABLE_CUDA=OFF -DENABLE_TENSORRT=OFF ..
cmake --build .
ctest --output-on-failure
```

### Build Modes

- **CPU-first default (CI-friendly)**: `-DENABLE_CUDA=OFF -DENABLE_TENSORRT=OFF`
- **Auto CUDA detection**: `-DENABLE_CUDA=ON -DENABLE_TENSORRT=OFF`
- **TensorRT runtime server**: `-DENABLE_CUDA=ON -DENABLE_TENSORRT=ON -DTENSORRT_ROOT=/path/to/tensorrt`

## 🔌 Broker Interoperability Boundary

Overhauled remains the GPU topology/placement/execution plane. YuKKi-OS remains the secure control plane.  
Integration boundary:

`YuKKi-OS control plane <-> broker protocol <-> overhauled placement/execution`

### Broker Protocol (Versioned)

- Magic/versioned frame: `BRK1`, protocol version `1`
- Length-prefixed request/response framing (4-byte big-endian length + bounded frame)
- Required request fields:
  - `task_id`, `source`, `destination`, `kind`, `priority`, `timeout_ms`, `payload`
- Response fields:
  - `task_id`, `status`, `selected_gpu`, `latency_ms`, `result`, `error`
- Validation:
  - bounded frame/payload/string sizes
  - non-empty routing fields
  - `timeout_ms > 0`

### Broker Server

Build target: `broker_server`

```bash
./build/broker_server 9090 --cpu-only
```

- `--cpu-only` provides deterministic fallback execution for tests and CPU nodes.
- Without `--cpu-only`, broker uses detected GPU topology and NVLink-aware placement when available.

### Security and Deployment Assumptions

- Broker transport is a bounded binary protocol with strict framing and validation.
- Oversized/malformed frames return structured protocol errors and are not blindly parsed.
- This repository does not embed YuKKi-OS code or create direct source dependencies across repositories.
- Deploy broker behind authenticated/authorized control-plane channels (mTLS/service mesh/proxy) for production.

## 📈 Performance

- **27% lower latency** vs blind round-robin
- **98% better tail latency** (p99)
- **94%+ GPU utilization** across all devices

## 📄 License

MIT License - see LICENSE file

## 🔗 References

- [NVIDIA CUDA P2P Documentation](https://docs.nvidia.com/cuda/cuda-runtime-api/)
- [NVLink Architecture](https://www.nvidia.com/en-us/data-center/nvlink/)
