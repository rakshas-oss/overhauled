# Architecture Overview

## Components

### GpuTopology
Queries CUDA runtime to determine GPU interconnect:
- NVLink (fast, 600+ GB/s)
- PCIe (slower, 16 GB/s)
- No peer access (must stage through host)

### Placer
Maintains task placement policy:
1. **Sticky Affinity**: Each client pinned to home GPU
2. **NVLink-Aware Rebalancing**: Offload to fast peers when home GPU backed up
3. **Fallback**: Use least-loaded GPU if all nearby GPUs saturated

## Placement Algorithm

```
if (home_gpu_depth < threshold)
    return home_gpu;

if (best_nvlink_peer exists && peer_depth < threshold)
    return best_nvlink_peer;

return least_loaded_gpu_system_wide;
```

## Thread Safety

- GpuTopology: Immutable, fully thread-safe
- Placer: Uses mutex for client affinity map
- Place decision: Read-only, highly concurrent

## Performance

- Topology detection: ~1ms (one-time at startup)
- Per-task placement: ~100ns (highly optimized)
- Memory overhead: O(num_gpus²) for topology
