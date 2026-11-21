// SPDX-License-Identifier: MIT
#include "worker_topology.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << "CHECK failed: " #condition << " at " << __FILE__ << ':' << __LINE__ << std::endl; \
        std::abort(); \
    } \
} while (false)

using namespace nvlink::broker;

static WorkerTopologySnapshot valid_topology() {
    WorkerTopologySnapshot topology;
    topology.allowed_cpus = {0, 1, 2, 3, 4, 5, 6, 7};
    topology.maximum_cpu_id = 7;
    topology.affinity_supported = false;
    topology.numa_nodes = {
        NumaNodeTopology{0, {0, 1, 2, 3}, {
            CpuGroupTopology{0, {0, 1}},
            CpuGroupTopology{1, {2, 3}},
        }},
        NumaNodeTopology{1, {4, 5, 6, 7}, {
            CpuGroupTopology{0, {4, 5}},
            CpuGroupTopology{1, {6, 7}},
        }},
    };
    return topology;
}

int main() {
    std::vector<uint32_t> parsed;
    std::string error;
    CHECK(parse_cpu_list("0-3,8,12-15", &parsed, &error));
    CHECK((parsed == std::vector<uint32_t>{0, 1, 2, 3, 8, 12, 13, 14, 15}));
    CHECK(!parse_cpu_list("0-3,3-4", &parsed, &error));
    CHECK(!parse_cpu_list("0,,2", &parsed, &error));
    CHECK(!parse_cpu_list("0,2,", &parsed, &error));
    CHECK(!parse_cpu_list("4-2", &parsed, &error));
    CHECK(!parse_cpu_list("-1,2", &parsed, &error));
    CHECK(!parse_cpu_list("0-1048576", &parsed, &error));

    auto topology = valid_topology();
    CHECK(validate_worker_topology(topology, &error));

    // Worker placement traverses the nested leaf domains but never escapes
    // the process-allowed CPU set or the selected NUMA parent.
    for (std::size_t ordinal = 0; ordinal < 32; ++ordinal) {
        const auto placement = worker_cpu_placement(topology, ordinal);
        CHECK(placement.has_value());
        CHECK(std::binary_search(topology.allowed_cpus.begin(),
                                 topology.allowed_cpus.end(), placement->cpu_id));
        const auto node = std::find_if(topology.numa_nodes.begin(), topology.numa_nodes.end(),
            [&placement](const NumaNodeTopology& candidate) {
                return candidate.id == placement->numa_node_id;
            });
        CHECK(node != topology.numa_nodes.end());
        CHECK(std::binary_search(node->cpus.begin(), node->cpus.end(), placement->cpu_id));
        const auto group = std::find_if(node->cpu_groups.begin(), node->cpu_groups.end(),
            [&placement](const CpuGroupTopology& candidate) {
                return candidate.id == placement->cpu_group_id;
            });
        CHECK(group != node->cpu_groups.end());
        CHECK(std::binary_search(group->cpus.begin(), group->cpus.end(), placement->cpu_id));
    }

    auto short_mask = valid_topology();
    short_mask.affinity_supported = true;
    short_mask.affinity_mask_bits = 7;
    short_mask.maximum_cpu_id = 7;
    CHECK(!validate_worker_topology(short_mask, &error));

    // A nested child cannot claim a CPU outside its NUMA parent.
    auto escaped_group = valid_topology();
    escaped_group.numa_nodes[0].cpu_groups[0].cpus = {0, 4};
    CHECK(!validate_worker_topology(escaped_group, &error));

    // Sibling CPU groups cannot overlap.
    auto overlapping_groups = valid_topology();
    overlapping_groups.numa_nodes[0].cpu_groups[1].cpus = {1, 2, 3};
    CHECK(!validate_worker_topology(overlapping_groups, &error));

    // Sibling NUMA domains cannot overlap.
    auto overlapping_nodes = valid_topology();
    overlapping_nodes.numa_nodes[1].cpus[0] = 3;
    CHECK(!validate_worker_topology(overlapping_nodes, &error));

    // Every process-allowed CPU must be represented exactly once.
    auto missing_cpu = valid_topology();
    missing_cpu.numa_nodes[1].cpu_groups[1].cpus.pop_back();
    CHECK(!validate_worker_topology(missing_cpu, &error));

    // A process affinity mask is the root security boundary. Topology cannot
    // claim CPUs the broker process itself is not allowed to use.
    auto escaped_root = valid_topology();
    escaped_root.numa_nodes[1].cpu_groups[1].cpus.push_back(99);
    escaped_root.numa_nodes[1].cpu_groups[1].cpus = {6, 7, 99};
    escaped_root.numa_nodes[1].cpus.push_back(99);
    escaped_root.allowed_cpus.pop_back();
    CHECK(!validate_worker_topology(escaped_root, &error));

    // CPU IDs above the legacy CPU_SETSIZE boundary remain representable.
    WorkerTopologySnapshot large_cpu;
    large_cpu.allowed_cpus = {2047, 2048, 4096};
    large_cpu.maximum_cpu_id = 4096;
    large_cpu.numa_nodes = {
        NumaNodeTopology{7, {2047, 2048, 4096}, {
            CpuGroupTopology{3, {2047, 2048, 4096}},
        }},
    };
    CHECK(validate_worker_topology(large_cpu, &error));
    const auto high_placement = worker_cpu_placement(large_cpu, 2);
    CHECK(high_placement.has_value() && high_placement->cpu_id == 4096);

    // Discovery must always return a valid bounded snapshot, including on
    // non-Linux platforms or restricted containers without affinity access.
    const auto discovered = discover_worker_topology();
    CHECK(validate_worker_topology(discovered, &error));
    CHECK(!discovered.allowed_cpus.empty());

    std::cout << "worker_topology_test passed\n";
    return 0;
}
