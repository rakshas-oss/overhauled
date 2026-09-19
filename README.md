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
- [SYSADMIN_HOWTO.md](docs/SYSADMIN_HOWTO.md) - Linux sysadmin guide for broker deployment and YuKKi-OS interoperability
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

Overhauled is the GPU topology, placement, and execution plane. `rakshas-oss/YuKKi-OS` is an external authenticated control plane. The interoperability boundary is the versioned broker protocol over TCP, not a direct source dependency or embedded runtime integration.

`YuKKi-OS control plane <-> authenticated proxy / mTLS boundary <-> BRK1 broker protocol <-> overhauled placement/execution`

### Broker Protocol (BRK1 / version 1)

Every broker message is sent as:

1. a 4-byte big-endian frame length
2. a bounded frame body beginning with:
   - 4-byte magic `BRK1`
   - 2-byte big-endian protocol version `1`
   - 1-byte message type (`1=request`, `2=response`)
   - 1 reserved byte (`0`)

Request body fields, in order:

- `task_id` (`uint16` length-prefixed string)
- `source` (`uint16` length-prefixed string)
- `destination` (`uint16` length-prefixed string)
- `kind` (`uint16` length-prefixed string)
- `priority` (`uint8`)
- `timeout_ms` (`uint32`, big-endian, must be `> 0`)
- `payload` (`uint32` length-prefixed binary blob)

Response body fields, in order:

- `task_id` (`uint16` length-prefixed string)
- `status` (`0=ok`, `1=rejected`, `2=error`, `3=timeout`)
- `selected_gpu` (`int32`; `-1` means CPU fallback)
- `latency_ms` (`uint64`)
- `result` (`uint32` length-prefixed binary blob)
- `error` (`uint16` length-prefixed string)

Default protocol limits are bounded and enforced before routing:

- max frame body: 1 MiB
- max payload/result blob: 512 KiB
- max string field: 1024 bytes

Validation rejects invalid magic, unsupported versions, wrong message types, trailing bytes, truncated frames, oversized strings/payloads, empty `task_id` / `source` / `destination` / `kind`, and `timeout_ms == 0`. Malformed requests receive structured broker rejections instead of being parsed opportunistically. See [docs/BROKER_PROTOCOL.md](docs/BROKER_PROTOCOL.md) for the protocol reference.

### Placement and execution behavior

- `broker_server` is the broker-facing executable built in this repository.
- `--cpu-only` disables GPU topology detection and always returns `selected_gpu = -1`, which is useful for CI, non-GPU nodes, and deterministic smoke tests.
- Without `--cpu-only`, the broker detects GPU topology once at startup and uses sticky affinity keyed by `source`, then queue-aware placement: home GPU first, then an NVLink-connected peer when beneficial, then least-loaded fallback.
- On systems without NVLink, placement still works and falls back to PCIe / least-loaded choices rather than requiring NVLink hardware.
- The default compute path used by current tests/examples expects the request payload to decode as a packed array of `double` values; the higher-level payload contract is otherwise application-specific and must be agreed between YuKKi-OS and the backend workload.

### Security assumptions and current limitations

- The broker transport is bounded and validated, but it does **not** provide authentication, authorization, or TLS on its own.
- Production deployments should place it behind an authenticated TCP proxy, mTLS sidecar, or service mesh, and should restrict listener access with normal host/network firewall policy.
- The server binds on all interfaces (`0.0.0.0`) for the configured port and exposes stdout/stderr logging only.
- There is no built-in HTTP health endpoint, metrics endpoint, audit log, or native service manager integration in this repository.
- Overhauled does not vendor YuKKi-OS code; the contract between the repositories is the BRK1/version 1 protocol and shared operational expectations only.

For deployment-oriented steps, see [docs/SYSADMIN_HOWTO.md](docs/SYSADMIN_HOWTO.md).

## 📈 Performance

- **27% lower latency** vs blind round-robin
- **98% better tail latency** (p99)
- **94%+ GPU utilization** across all devices

## 📄 License

MIT License - see LICENSE file

## 🔗 References

- [NVIDIA CUDA P2P Documentation](https://docs.nvidia.com/cuda/cuda-runtime-api/)
- [NVLink Architecture](https://www.nvidia.com/en-us/data-center/nvlink/)
