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
    // Detect topology
    auto topo = GpuTopology::detect();
    topo.enable_peer_access();
    
    // Create placer
    Placer placer(topo, 32);
    
    // Assign client home GPU
    int home = placer.assign_home(client_id);
    
    // Place task
    int gpu = placer.place(client_id, [&](int g) {
        return queues[g].size();
    });
    
    // Dispatch task to GPU
    queues[gpu].push(task);
}
```

## API Reference

### GpuTopology

**`static GpuTopology detect()`**
- Queries CUDA runtime for GPU topology
- Returns: GpuTopology object
- Throws: NVLinkError if no GPUs found

**`int num_gpus() const`**
- Returns: Number of GPUs

**`LinkType link(int from, int to) const`**
- Returns: Link type (SELF, NVLINK, PCIE, NONE)

**`int bandwidth_hint(int from, int to) const`**
- Returns: Bandwidth score (higher = faster)

**`int best_nvlink_peer(int gpu) const`**
- Returns: Best NVLink-connected peer GPU, or -1

**`void enable_peer_access() const`**
- Enables peer access for reachable GPU pairs
- Call once after detect()

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

## Examples

See `examples/` directory for complete examples.
