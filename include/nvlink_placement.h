#pragma once
// NVLink-Aware GPU Task Placement Library
// Public API for topology detection and NVLink-aware task placement.

#include <cstdint>
#include <vector>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>

namespace nvlink {

class NVLinkError : public std::runtime_error {
public:
    explicit NVLinkError(const std::string& msg) : std::runtime_error(msg) {}
};

enum class LinkType {
    SELF,       ///< Same device
    NVLINK,     ///< Direct NVLink interconnect
    PCIE,       ///< PCIe peer access
    NONE        ///< No peer access
};

inline const char* link_type_name(LinkType lt) {
    switch (lt) {
        case LinkType::SELF:   return "SELF";
        case LinkType::NVLINK: return "NVLINK";
        case LinkType::PCIE:   return "PCIE";
        case LinkType::NONE:   return "NONE";
        default:               return "UNKNOWN";
    }
}

class GpuTopology {
public:
    int num_gpus() const { return num_gpus_; }
    static GpuTopology detect();
    LinkType link(int from_gpu, int to_gpu) const;
    int bandwidth_hint(int from_gpu, int to_gpu) const;
    int best_nvlink_peer(int gpu) const;
    bool has_nvlink(int from_gpu, int to_gpu) const;
    bool can_access(int from_gpu, int to_gpu) const;
    void enable_peer_access() const;
    void print_info() const;
    std::string to_string() const;

    GpuTopology(const GpuTopology&) = delete;
    GpuTopology& operator=(const GpuTopology&) = delete;
    GpuTopology(GpuTopology&&) noexcept = default;
    GpuTopology& operator=(GpuTopology&&) noexcept = default;

private:
    friend class Placer;
    int num_gpus_;
    std::vector<std::vector<LinkType>> link_kind_;
    std::vector<std::vector<int>> link_bandwidth_;
    GpuTopology() = default;
};

class Placer {
public:
    explicit Placer(const GpuTopology& topo, size_t backlog_threshold = 32);
    ~Placer() = default;

    Placer(const Placer&) = delete;
    Placer& operator=(const Placer&) = delete;
    Placer(Placer&&) noexcept = default;
    Placer& operator=(Placer&&) noexcept = default;

    int assign_home(int client_id);
    int home_of(int client_id) const;
    
    template <typename QueueDepthFn>
    int place(int client_id, QueueDepthFn queue_depth) const;

    void release_client(int client_id) noexcept;
    size_t num_clients() const noexcept;

private:
    const GpuTopology& topo_;
    size_t backlog_threshold_;
    mutable std::atomic<uint64_t> rr_counter_;
    mutable std::unordered_map<int, int> affinity_;
    mutable std::mutex affinity_mutex_;
};

int get_gpu_count() noexcept;
bool is_valid_gpu(int gpu_idx) noexcept;
std::string get_gpu_name(int gpu_idx) noexcept;

template <typename QueueDepthFn>
int Placer::place(int client_id, QueueDepthFn queue_depth) const {
    std::lock_guard<std::mutex> lock(affinity_mutex_);
    auto it = affinity_.find(client_id);
    int home = (it != affinity_.end()) ? it->second : -1;

    if (home < 0 || home >= topo_.num_gpus()) {
        home = static_cast<int>(
            rr_counter_.fetch_add(1, std::memory_order_relaxed) % topo_.num_gpus()
        );
    }

    size_t home_depth = queue_depth(home);
    if (home_depth < backlog_threshold_) {
        return home;
    }

    int nvlink_peer = topo_.best_nvlink_peer(home);
    if (nvlink_peer >= 0 && queue_depth(nvlink_peer) < backlog_threshold_) {
        return nvlink_peer;
    }

    int best = home;
    size_t best_depth = home_depth;
    for (int i = 0; i < topo_.num_gpus(); ++i) {
        size_t d = queue_depth(i);
        if (d < best_depth) {
            best_depth = d;
            best = i;
        }
    }
    return best;
}

} // namespace nvlink
