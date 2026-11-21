// SPDX-License-Identifier: MIT
// Copyright (c) 2024 NVLink Placement Contributors
// Bounded topology discovery with validated machine -> NUMA -> CPU-group nesting.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "worker_topology.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <utility>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

namespace nvlink::broker {
namespace {

constexpr uint32_t kMaximumCpuId = static_cast<uint32_t>(kMaximumTopologyCpus - 1);

void set_error(std::string* error, const std::string& value) {
    if (error) *error = value;
}

bool sorted_unique_nonempty(const std::vector<uint32_t>& values) {
    return !values.empty() &&
           std::adjacent_find(values.begin(), values.end(),
               [](uint32_t a, uint32_t b) { return a >= b; }) == values.end();
}

bool is_subset(const std::vector<uint32_t>& child,
               const std::unordered_set<uint32_t>& parent) {
    return std::all_of(child.begin(), child.end(),
        [&parent](uint32_t cpu) { return parent.find(cpu) != parent.end(); });
}

std::vector<uint32_t> intersection(const std::vector<uint32_t>& left,
                                   const std::unordered_set<uint32_t>& right) {
    std::vector<uint32_t> result;
    for (uint32_t cpu : left)
        if (right.find(cpu) != right.end()) result.push_back(cpu);
    return result;
}

#if defined(__linux__)
std::vector<uint32_t> discover_allowed_cpus(bool* supported, std::size_t* mask_bits) {
    *supported = false;
    long configured = sysconf(_SC_NPROCESSORS_CONF);
    std::size_t bits = configured > 0
        ? static_cast<std::size_t>(configured)
        : static_cast<std::size_t>(128);
    bits = std::max<std::size_t>(bits, 128);

    while (bits <= kMaximumTopologyCpus) {
        const std::size_t bytes = CPU_ALLOC_SIZE(bits);
        cpu_set_t* mask = CPU_ALLOC(bits);
        if (!mask) return {};
        CPU_ZERO_S(bytes, mask);
        errno = 0;
        const int result = sched_getaffinity(0, bytes, mask);
        const int saved_errno = errno;
        if (result == 0) {
            std::vector<uint32_t> cpus;
            for (std::size_t cpu = 0; cpu < bits; ++cpu)
                if (CPU_ISSET_S(cpu, bytes, mask))
                    cpus.push_back(static_cast<uint32_t>(cpu));
            CPU_FREE(mask);
            if (!cpus.empty()) {
                *supported = true;
                *mask_bits = bits;
            }
            return cpus;
        }
        CPU_FREE(mask);
        if (saved_errno != EINVAL) return {};
        if (bits > kMaximumTopologyCpus / 2) break;
        bits *= 2;
    }
    return {};
}
#endif

std::vector<uint32_t> read_highest_cache_group(uint32_t cpu,
                                               const std::unordered_set<uint32_t>& node_cpus) {
#if defined(__linux__)
    const auto cache_root = std::filesystem::path("/sys/devices/system/cpu") /
                            ("cpu" + std::to_string(cpu)) / "cache";
    std::error_code ec;
    if (!std::filesystem::exists(cache_root, ec)) return {};
    int best_level = -1;
    std::vector<uint32_t> best;
    for (const auto& entry : std::filesystem::directory_iterator(cache_root, ec)) {
        if (ec) break;
        const std::string name = entry.path().filename().string();
        if (name.rfind("index", 0) != 0) continue;
        std::ifstream level_file(entry.path() / "level");
        std::ifstream shared_file(entry.path() / "shared_cpu_list");
        int level = -1;
        std::string shared_text;
        if (!(level_file >> level) || !(shared_file >> shared_text)) continue;
        if (level < best_level) continue;
        std::vector<uint32_t> shared;
        if (!parse_cpu_list(shared_text, &shared, nullptr)) continue;
        auto local = intersection(shared, node_cpus);
        if (local.empty() || !std::binary_search(local.begin(), local.end(), cpu)) continue;
        if (level > best_level || (level == best_level && local.size() > best.size())) {
            best_level = level;
            best = std::move(local);
        }
    }
    return best;
#else
    (void)cpu;
    (void)node_cpus;
    return {};
#endif
}

void build_cpu_groups(NumaNodeTopology* node) {
    std::unordered_set<uint32_t> node_set(node->cpus.begin(), node->cpus.end());
    std::map<std::vector<uint32_t>, std::vector<uint32_t>> groups;
    for (uint32_t cpu : node->cpus) {
        auto key = read_highest_cache_group(cpu, node_set);
        if (key.empty()) key = node->cpus;
        groups[key].push_back(cpu);
    }

    node->cpu_groups.clear();
    uint32_t group_id = 0;
    for (auto& item : groups) {
        CpuGroupTopology group;
        group.id = group_id++;
        group.cpus = std::move(item.second);
        node->cpu_groups.push_back(std::move(group));
    }
}

void collapse_to_one_numa_node(WorkerTopologySnapshot* topology) {
    NumaNodeTopology node;
    node.id = 0;
    node.cpus = topology->allowed_cpus;
    node.cpu_groups.push_back(CpuGroupTopology{0, node.cpus});
    topology->numa_nodes.clear();
    topology->numa_nodes.push_back(std::move(node));
}

} // namespace

bool parse_cpu_list(const std::string& text,
                    std::vector<uint32_t>* cpus,
                    std::string* error) {
    if (!cpus) {
        set_error(error, "CPU list output pointer is null");
        return false;
    }
    cpus->clear();
    if (text.empty() || text.size() > 8 * 1024 * 1024) {
        set_error(error, "CPU list is empty or exceeds the parser size bound");
        return false;
    }

    std::unordered_set<uint32_t> seen;
    std::size_t offset = 0;
    while (offset < text.size()) {
        const std::size_t comma = text.find(',', offset);
        const std::string token = text.substr(offset,
            comma == std::string::npos ? std::string::npos : comma - offset);
        if (token.empty()) {
            set_error(error, "CPU list contains an empty element");
            cpus->clear();
            return false;
        }
        const std::size_t dash = token.find('-');
        if (dash != std::string::npos && token.find('-', dash + 1) != std::string::npos) {
            set_error(error, "CPU range contains multiple separators");
            cpus->clear();
            return false;
        }
        try {
            std::size_t first_used = 0;
            const unsigned long long first_value =
                std::stoull(token.substr(0, dash), &first_used);
            if (first_used != (dash == std::string::npos ? token.size() : dash))
                throw std::invalid_argument("invalid CPU token");
            unsigned long long last_value = first_value;
            if (dash != std::string::npos) {
                const std::string tail = token.substr(dash + 1);
                std::size_t last_used = 0;
                last_value = std::stoull(tail, &last_used);
                if (last_used != tail.size()) throw std::invalid_argument("invalid CPU range");
            }
            if (last_value < first_value || last_value > kMaximumCpuId ||
                last_value - first_value >= kMaximumTopologyCpus) {
                set_error(error, "CPU range is reversed or exceeds the topology bound");
                cpus->clear();
                return false;
            }
            for (unsigned long long cpu = first_value; cpu <= last_value; ++cpu) {
                const auto id = static_cast<uint32_t>(cpu);
                if (!seen.insert(id).second) {
                    set_error(error, "CPU list contains duplicate CPU IDs");
                    cpus->clear();
                    return false;
                }
                cpus->push_back(id);
            }
        } catch (...) {
            set_error(error, "CPU list contains a malformed or negative CPU ID");
            cpus->clear();
            return false;
        }
        if (comma == std::string::npos) break;
        offset = comma + 1;
    }
    std::sort(cpus->begin(), cpus->end());
    return !cpus->empty();
}

bool validate_worker_topology(const WorkerTopologySnapshot& topology,
                              std::string* error) {
    if (!sorted_unique_nonempty(topology.allowed_cpus) ||
        topology.allowed_cpus.size() > kMaximumTopologyCpus) {
        set_error(error, "process-allowed CPU set is empty, unsorted, duplicated, or too large");
        return false;
    }
    if (topology.allowed_cpus.back() > kMaximumCpuId) {
        set_error(error, "process-allowed CPU ID exceeds the topology bound");
        return false;
    }
    if (topology.numa_nodes.empty() || topology.numa_nodes.size() > kMaximumNumaNodes) {
        set_error(error, "NUMA node count is outside the supported topology bound");
        return false;
    }

    std::unordered_set<uint32_t> allowed(
        topology.allowed_cpus.begin(), topology.allowed_cpus.end());
    std::unordered_set<uint32_t> seen_nodes;
    std::unordered_set<uint32_t> covered_cpus;
    std::size_t group_total = 0;

    for (const auto& node : topology.numa_nodes) {
        if (!seen_nodes.insert(node.id).second) {
            set_error(error, "NUMA node IDs are not unique");
            return false;
        }
        if (!sorted_unique_nonempty(node.cpus) || !is_subset(node.cpus, allowed)) {
            set_error(error, "NUMA child CPU set is empty, malformed, or escapes the process CPU set");
            return false;
        }
        for (uint32_t cpu : node.cpus) {
            if (!covered_cpus.insert(cpu).second) {
                set_error(error, "sibling NUMA CPU sets overlap");
                return false;
            }
        }
        if (node.cpu_groups.empty()) {
            set_error(error, "NUMA node has no CPU-group children");
            return false;
        }
        group_total += node.cpu_groups.size();
        if (group_total > kMaximumCpuGroups) {
            set_error(error, "CPU-group fanout exceeds the topology bound");
            return false;
        }
        std::unordered_set<uint32_t> node_set(node.cpus.begin(), node.cpus.end());
        std::unordered_set<uint32_t> seen_group_ids;
        std::unordered_set<uint32_t> group_coverage;
        for (const auto& group : node.cpu_groups) {
            if (!seen_group_ids.insert(group.id).second) {
                set_error(error, "CPU-group IDs are not unique within their NUMA parent");
                return false;
            }
            if (!sorted_unique_nonempty(group.cpus) || !is_subset(group.cpus, node_set)) {
                set_error(error, "CPU-group child set is empty, malformed, or escapes its NUMA parent");
                return false;
            }
            for (uint32_t cpu : group.cpus) {
                if (!group_coverage.insert(cpu).second) {
                    set_error(error, "sibling CPU-group sets overlap");
                    return false;
                }
            }
        }
        if (group_coverage.size() != node_set.size()) {
            set_error(error, "CPU groups do not exactly cover their NUMA parent");
            return false;
        }
    }

    if (covered_cpus.size() != allowed.size()) {
        set_error(error, "NUMA children do not exactly cover the process-allowed CPU set");
        return false;
    }
    set_error(error, "");
    return true;
}

static WorkerTopologySnapshot discover_worker_topology_uncached() {
    WorkerTopologySnapshot topology;
#if defined(__linux__)
    topology.allowed_cpus = discover_allowed_cpus(&topology.affinity_supported,
                                                   &topology.affinity_mask_bits);
#endif
    if (topology.allowed_cpus.empty()) {
        std::size_t count = 1;
#if defined(__linux__)
        const long configured = sysconf(_SC_NPROCESSORS_CONF);
        if (configured > 0) count = static_cast<std::size_t>(configured);
#else
        count = static_cast<std::size_t>(std::max(1u, std::thread::hardware_concurrency()));
#endif
        count = std::min(count, kMaximumTopologyCpus);
        topology.allowed_cpus.reserve(count);
        for (std::size_t cpu = 0; cpu < count; ++cpu)
            topology.allowed_cpus.push_back(static_cast<uint32_t>(cpu));
        topology.affinity_supported = false;
        topology.affinity_mask_bits = 0;
    }
    topology.maximum_cpu_id = topology.allowed_cpus.back();

    // Count unique physical cores where Linux exports package/core IDs. This
    // is diagnostic and sizing guidance only; CPU affinity remains the root
    // authority for all worker placements.
#if defined(__linux__)
    {
        std::set<std::pair<long, long>> physical_cores;
        for (uint32_t cpu : topology.allowed_cpus) {
            const auto cpu_root = std::filesystem::path("/sys/devices/system/cpu") /
                                  ("cpu" + std::to_string(cpu)) / "topology";
            std::ifstream package_file(cpu_root / "physical_package_id");
            std::ifstream core_file(cpu_root / "core_id");
            long package_id = -1;
            long core_id = -1;
            if (package_file >> package_id && core_file >> core_id)
                physical_cores.emplace(package_id, core_id);
        }
        topology.physical_core_count = physical_cores.empty()
            ? topology.allowed_cpus.size() : physical_cores.size();
    }
#else
    topology.physical_core_count = topology.allowed_cpus.size();
#endif

#if defined(__linux__)
    const std::filesystem::path root("/sys/devices/system/node");
    std::error_code ec;
    std::unordered_set<uint32_t> allowed(
        topology.allowed_cpus.begin(), topology.allowed_cpus.end());
    if (std::filesystem::exists(root, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
            if (ec || topology.numa_nodes.size() >= kMaximumNumaNodes) break;
            const std::string name = entry.path().filename().string();
            if (name.rfind("node", 0) != 0) continue;
            uint32_t node_id = 0;
            try {
                const auto parsed = std::stoul(name.substr(4));
                if (parsed > UINT32_MAX) continue;
                node_id = static_cast<uint32_t>(parsed);
            } catch (...) { continue; }
            std::ifstream input(entry.path() / "cpulist");
            std::string cpu_text;
            if (!input || !std::getline(input, cpu_text)) continue;
            std::vector<uint32_t> listed;
            if (!parse_cpu_list(cpu_text, &listed, nullptr)) continue;
            NumaNodeTopology node;
            node.id = node_id;
            node.cpus = intersection(listed, allowed);
            if (node.cpus.empty()) continue;
            build_cpu_groups(&node);
            topology.numa_nodes.push_back(std::move(node));
        }
    }
#endif

    std::sort(topology.numa_nodes.begin(), topology.numa_nodes.end(),
        [](const NumaNodeTopology& a, const NumaNodeTopology& b) { return a.id < b.id; });

    std::string validation_error;
    if (!validate_worker_topology(topology, &validation_error)) {
        // Cache topology can be incomplete in virtualized environments. Keep
        // the validated NUMA parents but collapse each leaf to one group first.
        for (auto& node : topology.numa_nodes) {
            node.cpu_groups.clear();
            node.cpu_groups.push_back(CpuGroupTopology{0, node.cpus});
        }
        if (!validate_worker_topology(topology, &validation_error)) {
            // If parent metadata is also incomplete, fail closed to one local
            // domain rather than creating overlapping or orphaned children.
            collapse_to_one_numa_node(&topology);
        }
    }
    return topology;
}

WorkerTopologySnapshot discover_worker_topology() {
    // The process cpuset and NUMA tree are treated as an immutable startup
    // security boundary. All scheduler components share the same validated
    // snapshot; hotplug/cpuset changes require process restart.
    static const WorkerTopologySnapshot cached = discover_worker_topology_uncached();
    return cached;
}

std::optional<WorkerCpuPlacement> worker_cpu_placement(
    const WorkerTopologySnapshot& topology,
    std::size_t worker_ordinal) noexcept {
    if (topology.allowed_cpus.empty() || topology.numa_nodes.empty()) return std::nullopt;
    std::size_t total_cpus = 0;
    for (const auto& node : topology.numa_nodes) total_cpus += node.cpus.size();
    if (total_cpus == 0) return std::nullopt;
    std::size_t slot = worker_ordinal % total_cpus;
    for (const auto& node : topology.numa_nodes) {
        if (slot >= node.cpus.size()) {
            slot -= node.cpus.size();
            continue;
        }
        std::size_t local_slot = slot;
        for (const auto& group : node.cpu_groups) {
            if (local_slot >= group.cpus.size()) {
                local_slot -= group.cpus.size();
                continue;
            }
            return WorkerCpuPlacement{node.id, group.id, group.cpus[local_slot]};
        }
        return std::nullopt;
    }
    return std::nullopt;
}

bool pin_current_thread_to_cpu(uint32_t cpu_id,
                               uint32_t maximum_cpu_id,
                               std::size_t affinity_mask_bits) noexcept {
#if defined(__linux__)
    if (cpu_id > maximum_cpu_id || maximum_cpu_id >= kMaximumTopologyCpus) return false;
    const std::size_t bits = std::max<std::size_t>(
        static_cast<std::size_t>(maximum_cpu_id) + 1, affinity_mask_bits);
    if (bits == 0 || bits > kMaximumTopologyCpus) return false;
    const std::size_t bytes = CPU_ALLOC_SIZE(bits);
    cpu_set_t* mask = CPU_ALLOC(bits);
    if (!mask) return false;
    CPU_ZERO_S(bytes, mask);
    CPU_SET_S(cpu_id, bytes, mask);
    const int result = pthread_setaffinity_np(pthread_self(), bytes, mask);
    CPU_FREE(mask);
    return result == 0;
#else
    (void)cpu_id;
    (void)maximum_cpu_id;
    (void)affinity_mask_bits;
    return false;
#endif
}

} // namespace nvlink::broker
