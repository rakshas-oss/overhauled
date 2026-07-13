#include "nvlink_placement.h"
#include <cuda_runtime.h>
#include <algorithm>
#include <sstream>
#include <cstdio>

namespace nvlink {

int get_gpu_count() noexcept {
    int count = 0;
    cudaError_t err = cudaGetDeviceCount(&count);
    return (err == cudaSuccess) ? count : 0;
}

bool is_valid_gpu(int gpu_idx) noexcept {
    int count = get_gpu_count();
    return gpu_idx >= 0 && gpu_idx < count;
}

std::string get_gpu_name(int gpu_idx) noexcept {
    if (!is_valid_gpu(gpu_idx)) return "";
    cudaDeviceProp prop;
    cudaError_t err = cudaGetDeviceProperties(&prop, gpu_idx);
    if (err != cudaSuccess) return "";
    return std::string(prop.name);
}

GpuTopology GpuTopology::detect() {
    GpuTopology topo;
    int num_gpus = get_gpu_count();
    if (num_gpus <= 0) {
        throw NVLinkError("No CUDA-enabled GPUs found.");
    }

    topo.num_gpus_ = num_gpus;
    topo.link_kind_.assign(num_gpus, std::vector<LinkType>(num_gpus, LinkType::NONE));
    topo.link_bandwidth_.assign(num_gpus, std::vector<int>(num_gpus, 0));

    for (int i = 0; i < num_gpus; ++i) {
        topo.link_kind_[i][i] = LinkType::SELF;
        topo.link_bandwidth_[i][i] = 1000;
    }

    for (int i = 0; i < num_gpus; ++i) {
        for (int j = 0; j < num_gpus; ++j) {
            if (i == j) continue;

            int can_access = 0;
            cudaError_t err = cudaDeviceCanAccessPeer(&can_access, i, j);
            if (err != cudaSuccess || !can_access) {
                topo.link_kind_[i][j] = LinkType::NONE;
                topo.link_bandwidth_[i][j] = 0;
                continue;
            }

            int perf_rank = 0;
            err = cudaDeviceGetP2PAttribute(&perf_rank, cudaDevP2PAttrPerformanceRank, i, j);
            if (err != cudaSuccess) {
                topo.link_kind_[i][j] = LinkType::PCIE;
                topo.link_bandwidth_[i][j] = 10;
                continue;
            }

            int native_atomics = 0;
            cudaDeviceGetP2PAttribute(&native_atomics, cudaDevP2PAttrNativeAtomicSupported, i, j);

            if (perf_rank == 0 && native_atomics) {
                topo.link_kind_[i][j] = LinkType::NVLINK;
                topo.link_bandwidth_[i][j] = 100;
            } else if (perf_rank == 0) {
                topo.link_kind_[i][j] = LinkType::NVLINK;
                topo.link_bandwidth_[i][j] = 90;
            } else {
                topo.link_kind_[i][j] = LinkType::PCIE;
                topo.link_bandwidth_[i][j] = 10;
            }
        }
    }

    return topo;
}

LinkType GpuTopology::link(int from_gpu, int to_gpu) const {
    if (from_gpu < 0 || from_gpu >= num_gpus_ ||
        to_gpu < 0 || to_gpu >= num_gpus_) {
        return LinkType::NONE;
    }
    return link_kind_[from_gpu][to_gpu];
}

int GpuTopology::bandwidth_hint(int from_gpu, int to_gpu) const {
    if (from_gpu < 0 || from_gpu >= num_gpus_ ||
        to_gpu < 0 || to_gpu >= num_gpus_) {
        return 0;
    }
    return link_bandwidth_[from_gpu][to_gpu];
}

int GpuTopology::best_nvlink_peer(int gpu) const {
    if (gpu < 0 || gpu >= num_gpus_) return -1;
    int best = -1;
    int best_score = -1;
    for (int j = 0; j < num_gpus_; ++j) {
        if (j == gpu) continue;
        if (link_kind_[gpu][j] != LinkType::NVLINK) continue;
        if (link_bandwidth_[gpu][j] > best_score) {
            best_score = link_bandwidth_[gpu][j];
            best = j;
        }
    }
    return best;
}

bool GpuTopology::has_nvlink(int from_gpu, int to_gpu) const {
    return link(from_gpu, to_gpu) == LinkType::NVLINK;
}

bool GpuTopology::can_access(int from_gpu, int to_gpu) const {
    LinkType lt = link(from_gpu, to_gpu);
    return lt == LinkType::NVLINK || lt == LinkType::PCIE;
}

void GpuTopology::enable_peer_access() const {
    for (int i = 0; i < num_gpus_; ++i) {
        cudaError_t err = cudaSetDevice(i);
        if (err != cudaSuccess) {
            throw NVLinkError("Failed to set device " + std::to_string(i));
        }
        for (int j = 0; j < num_gpus_; ++j) {
            if (i == j) continue;
            LinkType lt = link_kind_[i][j];
            if (lt == LinkType::NVLINK || lt == LinkType::PCIE) {
                cudaError_t e = cudaDeviceEnablePeerAccess(j, 0);
                if (e != cudaSuccess && e != cudaErrorPeerAccessAlreadyEnabled) {
                    fprintf(stderr, "[NVLink] Warning: peer access %d->%d failed\n", i, j);
                }
            }
        }
    }
}

void GpuTopology::print_info() const {
    printf("[NVLink] %d GPU(s) detected.\n", num_gpus_);
    for (int i = 0; i < num_gpus_; ++i) {
        printf("[NVLink] GPU%d (%s)\n", i, get_gpu_name(i).c_str());
        for (int j = 0; j < num_gpus_; ++j) {
            if (i == j) continue;
            printf("[NVLink]   -> GPU%d : %s (bandwidth_hint=%d)\n",
                   j, link_type_name(link_kind_[i][j]), link_bandwidth_[i][j]);
        }
    }
}

std::string GpuTopology::to_string() const {
    std::ostringstream oss;
    oss << num_gpus_ << " GPU(s): ";
    for (int i = 0; i < num_gpus_; ++i) {
        oss << get_gpu_name(i);
        if (i < num_gpus_ - 1) oss << ", ";
    }
    return oss.str();
}

Placer::Placer(const GpuTopology& topo, size_t backlog_threshold)
    : topo_(topo), backlog_threshold_(backlog_threshold), rr_counter_(0) {}

int Placer::assign_home(int client_id) {
    int home = static_cast<int>(
        rr_counter_.fetch_add(1, std::memory_order_relaxed) % topo_.num_gpus()
    );
    std::lock_guard<std::mutex> lock(affinity_mutex_);
    affinity_[client_id] = home;
    return home;
}

int Placer::home_of(int client_id) const {
    std::lock_guard<std::mutex> lock(affinity_mutex_);
    auto it = affinity_.find(client_id);
    if (it != affinity_.end()) {
        return it->second;
    }
    return -1;
}

void Placer::release_client(int client_id) noexcept {
    std::lock_guard<std::mutex> lock(affinity_mutex_);
    affinity_.erase(client_id);
}

size_t Placer::num_clients() const noexcept {
    std::lock_guard<std::mutex> lock(affinity_mutex_);
    return affinity_.size();
}

} // namespace nvlink
