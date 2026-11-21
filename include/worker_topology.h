// SPDX-License-Identifier: MIT
// Copyright (c) 2024 NVLink Placement Contributors
// Bounded, process-visible CPU topology and nested placement domains.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nvlink::broker {

constexpr std::size_t kMaximumTopologyCpus = 1'048'576;
constexpr std::size_t kMaximumNumaNodes = 4'096;
constexpr std::size_t kMaximumCpuGroups = 65'536;

struct CpuGroupTopology {
    uint32_t id = 0;
    std::vector<uint32_t> cpus;
};

struct NumaNodeTopology {
    uint32_t id = 0;
    std::vector<uint32_t> cpus;
    // Leaf placement domains (normally the highest-level shared CPU cache).
    std::vector<CpuGroupTopology> cpu_groups;
};

struct WorkerTopologySnapshot {
    // CPUs allowed to this process, not all CPUs installed in the host.
    std::vector<uint32_t> allowed_cpus;
    std::vector<NumaNodeTopology> numa_nodes;
    bool affinity_supported = false;
    uint32_t maximum_cpu_id = 0;
    std::size_t affinity_mask_bits = 0;
    std::size_t physical_core_count = 0;
};

struct WorkerCpuPlacement {
    uint32_t numa_node_id = 0;
    uint32_t cpu_group_id = 0;
    uint32_t cpu_id = 0;
};

// Parse Linux cpulist syntax such as "0-3,8,12-15". Reject malformed,
// duplicate, out-of-range, and unbounded input rather than broadening a mask.
bool parse_cpu_list(const std::string& text,
                    std::vector<uint32_t>* cpus,
                    std::string* error = nullptr);

// Validate machine -> NUMA -> CPU-group containment, sibling disjointness,
// bounded fanout, and exact coverage of the process-allowed CPU set.
bool validate_worker_topology(const WorkerTopologySnapshot& topology,
                              std::string* error = nullptr);

WorkerTopologySnapshot discover_worker_topology();

// Deterministically place worker ordinal over allowed CPU groups in proportion
// to their CPU capacity. No placement can reference a CPU outside the snapshot.
std::optional<WorkerCpuPlacement> worker_cpu_placement(
    const WorkerTopologySnapshot& topology,
    std::size_t worker_ordinal) noexcept;

// Apply a single-CPU dynamic affinity mask. Supports CPU IDs beyond CPU_SETSIZE.
bool pin_current_thread_to_cpu(uint32_t cpu_id,
                               uint32_t maximum_cpu_id,
                               std::size_t affinity_mask_bits) noexcept;

} // namespace nvlink::broker
