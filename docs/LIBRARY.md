# NVLink Placement Library - Usage Guide

## Quick Start

### Installation

```bash
mkdir build && cd build
cmake ..
make
sudo make install
```

### Minimal Integration

```cpp
#include "nvlink_placement.h"

using namespace nvlink;

int main() {
    auto topo = GpuTopology::detect();
    topo.enable_peer_access();

    Placer placer(topo, 32);

    int home = placer.assign_home(client_id);
    int gpu = placer.place(client_id, [&](int g) {
        return queues[g].size();
    });

    queues[gpu].push(task);
}
```

## ADI Protocol Server

The library also exposes `#include "adi_server.h"` for embedding a reusable ADI binary protocol server.

- Request framing: 4-byte big-endian length prefix + 40-byte payload (5 big-endian doubles)
- Response framing: 13-byte header + 40-byte payload
- Response sequence: first exchange sends Primary; later exchanges send Primary plus Delta using the same sequence id
- Placement policy: sticky home GPU, then NVLink peer offload, then least-loaded fallback
- Execution model: one TCP accept loop, one worker per GPU, one session thread per client
- Compute path: custom callback or the default scaling implementation

### ADI Server Example

```cpp
#include "adi_server.h"

int main() {
    auto topo = nvlink::GpuTopology::detect();
    topo.enable_peer_access();

    nvlink::adi::AdiServerConfig config;
    config.port = 8080;
    config.verbose = true;

    nvlink::adi::AdiServer server(topo, config);
    server.run();
}
```

## API Reference

### GpuTopology

**`static GpuTopology detect()`**
- Queries CUDA runtime for GPU topology
- Returns: `GpuTopology` object
- Throws: `NVLinkError` if no GPUs are found

**`int num_gpus() const`**
- Returns: Number of GPUs

**`LinkType link(int from, int to) const`**
- Returns: Link type (`SELF`, `NVLINK`, `PCIE`, `NONE`)

**`int bandwidth_hint(int from, int to) const`**
- Returns: Bandwidth score (higher = faster)

**`int best_nvlink_peer(int gpu) const`**
- Returns: Best NVLink-connected peer GPU, or `-1`

**`void enable_peer_access() const`**
- Enables peer access for reachable GPU pairs
- Call once after `detect()`

**`void print_info() const`**
- Prints topology to stdout

### Placer

**`explicit Placer(const GpuTopology& topo, size_t backlog_threshold = 32)`**
- Creates placement policy
- `backlog_threshold`: Queue depth for rebalancing (default 32)

**`int assign_home(int client_id)`**
- Assigns home GPU to client
- Called once on client connect
- Returns: Home GPU index

**`int place(int client_id, QueueDepthFn queue_depth)`**
- Decides GPU for task
- `queue_depth`: Lambda returning queue depth for GPU
- Returns: Target GPU index

**`void release_client(int client_id) noexcept`**
- Removes client from tracking
- Call on client disconnect

**`size_t num_clients() const noexcept`**
- Returns: Current tracked client count

### ADI Server

**`explicit AdiServer(const GpuTopology& topology, const AdiServerConfig& config = {})`**
- Creates the ADI server with a reusable `nvlink::Placer`

**`void run()`**
- Starts the blocking TCP listener and GPU worker pool

**`void stop() noexcept`**
- Requests graceful shutdown and joins worker/session threads

**`size_t active_clients() const noexcept`**
- Returns the number of connected clients

## Examples

See `examples/` directory for complete examples.
