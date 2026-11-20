// SPDX-License-Identifier: MIT
// YuKKi-directed class-aware worker pools and Linux NUMA CPU affinity.

#include "worker_scheduler.h"

#include <algorithm>
#include <atomic>
#include <new>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

namespace nvlink::broker {

const char* worker_class_name(WorkerClass value) noexcept {
    switch (value) {
        case WorkerClass::Control: return "control";
        case WorkerClass::RealtimeSpatial: return "realtime-spatial";
        case WorkerClass::Throughput: return "throughput";
        case WorkerClass::ExactCpu: return "exact-cpu";
        case WorkerClass::Inference: return "inference";
    }
    return "throughput";
}

bool parse_worker_class(const std::string& value, WorkerClass* out) noexcept {
    if (out == nullptr) return false;
    if (value == "control") *out = WorkerClass::Control;
    else if (value == "realtime-spatial") *out = WorkerClass::RealtimeSpatial;
    else if (value == "throughput") *out = WorkerClass::Throughput;
    else if (value == "exact-cpu") *out = WorkerClass::ExactCpu;
    else if (value == "inference") *out = WorkerClass::Inference;
    else return false;
    return true;
}


WorkerAdmissionBudget::WorkerAdmissionBudget(std::size_t max_outstanding_requests,
                                             std::size_t max_payload_bytes)
    : max_outstanding_requests_(max_outstanding_requests),
      max_payload_bytes_(max_payload_bytes) {
    if (max_outstanding_requests_ == 0 || max_payload_bytes_ == 0)
        throw std::invalid_argument("worker admission budgets must be greater than zero");
}

bool WorkerAdmissionBudget::try_acquire(std::size_t payload_bytes) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (outstanding_requests_ >= max_outstanding_requests_ ||
        payload_bytes > max_payload_bytes_ - outstanding_payload_bytes_) return false;
    ++outstanding_requests_;
    outstanding_payload_bytes_ += payload_bytes;
    return true;
}

void WorkerAdmissionBudget::release(std::size_t payload_bytes) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (outstanding_requests_ > 0) --outstanding_requests_;
    outstanding_payload_bytes_ = payload_bytes > outstanding_payload_bytes_
        ? 0 : outstanding_payload_bytes_ - payload_bytes;
}

std::size_t WorkerAdmissionBudget::outstanding_requests() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return outstanding_requests_;
}

std::size_t WorkerAdmissionBudget::outstanding_payload_bytes() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return outstanding_payload_bytes_;
}

namespace {
using Clock = std::chrono::steady_clock;

std::size_t class_index(WorkerClass value) {
    const auto index = static_cast<std::size_t>(value);
    if (index >= kWorkerClassCount) throw std::invalid_argument("invalid worker class");
    return index;
}

struct CpuNode {
    uint32_t id = 0;
    std::vector<int> cpus;
};

std::vector<int> parse_cpu_list(const std::string& text) {
    std::vector<int> result;
    std::size_t offset = 0;
    while (offset < text.size()) {
        const std::size_t comma = text.find(',', offset);
        const std::string part = text.substr(offset, comma == std::string::npos
                                                        ? std::string::npos
                                                        : comma - offset);
        const std::size_t dash = part.find('-');
        try {
            const long first = std::stol(part.substr(0, dash));
            const long last = dash == std::string::npos ? first : std::stol(part.substr(dash + 1));
            if (first < 0 || last < first || last >= 1000000 || last - first > 65535) return {};
            for (long cpu = first; cpu <= last; ++cpu) result.push_back(static_cast<int>(cpu));
        } catch (...) {
            return {};
        }
        if (comma == std::string::npos) break;
        offset = comma + 1;
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

class CpuNumaTopology {
public:
    CpuNumaTopology() {
#if defined(__linux__)
        cpu_set_t allowed;
        CPU_ZERO(&allowed);
        if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) return;

        const std::filesystem::path root("/sys/devices/system/node");
        std::error_code ec;
        if (std::filesystem::exists(root, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
                if (ec) break;
                const std::string name = entry.path().filename().string();
                if (name.rfind("node", 0) != 0) continue;
                uint32_t node_id = 0;
                try { node_id = static_cast<uint32_t>(std::stoul(name.substr(4))); }
                catch (...) { continue; }
                std::ifstream input(entry.path() / "cpulist");
                std::string cpu_text;
                if (!input || !std::getline(input, cpu_text)) continue;
                CpuNode node;
                node.id = node_id;
                for (int cpu : parse_cpu_list(cpu_text)) {
                    if (cpu >= 0 && cpu < CPU_SETSIZE && CPU_ISSET(cpu, &allowed))
                        node.cpus.push_back(cpu);
                }
                if (!node.cpus.empty()) nodes_.push_back(std::move(node));
            }
        }
        std::sort(nodes_.begin(), nodes_.end(),
                  [](const CpuNode& a, const CpuNode& b) { return a.id < b.id; });
        if (nodes_.empty()) {
            CpuNode fallback;
            fallback.id = 0;
            for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
                if (CPU_ISSET(cpu, &allowed)) fallback.cpus.push_back(cpu);
            if (!fallback.cpus.empty()) nodes_.push_back(std::move(fallback));
        }
#endif
    }

    // CPU choice is stable for a worker on a given node. Avoid repeating the
    // pthread affinity syscall when consecutive tasks resolve to the same CPU.
    std::optional<uint32_t> pin(
        std::optional<uint32_t> requested_node,
        std::optional<uint64_t> affinity_key,
        std::size_t worker_index,
        std::optional<std::pair<uint32_t, int>>& current_pin) const noexcept {
#if defined(__linux__)
        if (nodes_.empty()) return std::nullopt;
        const CpuNode* selected = nullptr;
        if (requested_node) {
            for (const auto& node : nodes_) if (node.id == *requested_node) {
                selected = &node;
                break;
            }
        }
        if (selected == nullptr) {
            const uint64_t slot = affinity_key
                ? (*affinity_key % nodes_.size())
                : (worker_index % nodes_.size());
            selected = &nodes_[static_cast<std::size_t>(slot)];
        }
        if (selected->cpus.empty()) return std::nullopt;
        const int cpu = selected->cpus[worker_index % selected->cpus.size()];
        if (current_pin && current_pin->first == selected->id &&
            current_pin->second == cpu) return selected->id;
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET(cpu, &mask);
        if (pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask) != 0) {
            current_pin.reset();
            return std::nullopt;
        }
        current_pin = std::make_pair(selected->id, cpu);
        return selected->id;
#else
        (void)requested_node;
        (void)affinity_key;
        (void)worker_index;
        (void)current_pin;
        return std::nullopt;
#endif
    }

private:
    std::vector<CpuNode> nodes_;
};
} // namespace

class ClassWorkerPool::Impl {
public:
    explicit Impl(const WorkerPoolConfig& config) {
        std::size_t total = 0;
        for (const std::size_t count : config.workers) {
            if (count == 0) throw std::invalid_argument("every worker class needs at least one worker");
            if (count > 256 || total > 256 - count)
                throw std::invalid_argument("total worker count must be in 1..256");
            total += count;
        }

        try {
            for (std::size_t class_id = 0; class_id < kWorkerClassCount; ++class_id) {
                threads_[class_id].reserve(config.workers[class_id]);
                for (std::size_t worker = 0; worker < config.workers[class_id]; ++worker) {
                    threads_[class_id].emplace_back(
                        [this, class_id, worker] { worker_loop(class_id, worker); });
                }
            }
        } catch (...) {
            stop();
            join();
            throw;
        }
    }

    ~Impl() { stop(); join(); }

    bool enqueue(WorkerClass worker_class,
                 std::optional<uint64_t> affinity_key,
                 std::optional<uint32_t> preferred_node,
                 Task task) {
        return enqueue_with_deadline(worker_class, affinity_key, preferred_node,
                                     std::nullopt, std::move(task));
    }

    bool enqueue_with_deadline(
        WorkerClass worker_class,
        std::optional<uint64_t> affinity_key,
        std::optional<uint32_t> preferred_node,
        std::optional<Clock::time_point> deadline,
        Task task) {
        if (!task) return false;
        const std::size_t index = class_index(worker_class);
        auto& queue = queues_[index];
        {
            // Each class owns its queue lock. Traffic in one class no longer
            // serialises admission or wakes workers belonging to other classes.
            std::lock_guard<std::mutex> lock(queue.mutex);
            if (stopping_.load(std::memory_order_acquire)) return false;
            QueuedTask queued_task{
                Clock::now(), affinity_key, preferred_node, deadline,
                queue.next_sequence++, std::move(task)};
            try {
                queue.tasks.insert(std::move(queued_task));
            } catch (const std::bad_alloc&) {
                return false;
            }
        }
        queue.cv.notify_one();
        return true;
    }

    void stop() noexcept {
        stopping_.store(true, std::memory_order_release);
        for (auto& queue : queues_) {
            // Synchronise with any enqueue already inside this class's critical
            // section before waking workers to drain and exit.
            {
                std::lock_guard<std::mutex> lock(queue.mutex);
            }
            queue.cv.notify_all();
        }
    }

    void join() noexcept {
        std::lock_guard<std::mutex> join_lock(join_mutex_);
        for (auto& pool : threads_) {
            for (auto& worker : pool) if (worker.joinable()) worker.join();
            pool.clear();
        }
    }

    std::size_t queued(WorkerClass worker_class) const noexcept {
        try {
            const std::size_t index = class_index(worker_class);
            const auto& queue = queues_[index];
            std::lock_guard<std::mutex> lock(queue.mutex);
            return queue.tasks.size();
        } catch (...) {
            return 0;
        }
    }

private:
    struct QueuedTask {
        Clock::time_point enqueued_at{};
        std::optional<uint64_t> affinity_key;
        std::optional<uint32_t> preferred_node;
        std::optional<Clock::time_point> deadline;
        uint64_t sequence = 0;
        Task task;
    };

    struct QueuedTaskCompare {
        bool operator()(const QueuedTask& left, const QueuedTask& right) const noexcept {
            if (left.deadline && right.deadline && *left.deadline != *right.deadline)
                return *left.deadline < *right.deadline;
            if (left.deadline.has_value() != right.deadline.has_value())
                return left.deadline.has_value();
            return left.sequence < right.sequence;
        }
    };

    struct ClassQueue {
        mutable std::mutex mutex;
        std::condition_variable cv;
        // Ordered insertion gives O(log n) deadline admission and preserves
        // FIFO order for equal deadlines and all non-deadline classes.
        std::multiset<QueuedTask, QueuedTaskCompare> tasks;
        uint64_t next_sequence = 0;
    };

    void worker_loop(std::size_t class_id, std::size_t worker_index) noexcept {
        const WorkerClass worker_class = static_cast<WorkerClass>(class_id);
        std::size_t global_worker_index = worker_index;
        for (std::size_t prior_class = 0; prior_class < class_id; ++prior_class)
            global_worker_index += threads_[prior_class].size();

        auto& queue = queues_[class_id];
        std::optional<std::pair<uint32_t, int>> current_pin;
        for (;;) {
            QueuedTask queued_task;
            {
                std::unique_lock<std::mutex> lock(queue.mutex);
                queue.cv.wait(lock, [this, &queue] {
                    return stopping_.load(std::memory_order_acquire) || !queue.tasks.empty();
                });
                if (stopping_.load(std::memory_order_acquire) && queue.tasks.empty()) return;
                auto node = queue.tasks.extract(queue.tasks.begin());
                queued_task = std::move(node.value());
            }

            const auto started = Clock::now();
            const auto wait = std::chrono::duration_cast<std::chrono::microseconds>(
                started - queued_task.enqueued_at).count();
            WorkerExecutionContext context;
            context.worker_class = worker_class;
            context.worker_id = static_cast<uint32_t>(class_id * 256 + worker_index);
            context.queue_wait_us = wait > 0 ? static_cast<uint64_t>(wait) : 0;
            context.numa_node = topology_.pin(
                queued_task.preferred_node, queued_task.affinity_key,
                global_worker_index, current_pin);
            try { queued_task.task(context); }
            catch (...) {
                // A task failure is isolated to this worker and its client completion guard.
            }
        }
    }

    std::mutex join_mutex_;
    std::array<ClassQueue, kWorkerClassCount> queues_;
    std::array<std::vector<std::thread>, kWorkerClassCount> threads_;
    CpuNumaTopology topology_;
    std::atomic<bool> stopping_{false};
};

ClassWorkerPool::ClassWorkerPool(const WorkerPoolConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}
ClassWorkerPool::~ClassWorkerPool() = default;

bool ClassWorkerPool::enqueue(WorkerClass worker_class,
                              std::optional<uint64_t> spatial_affinity_key,
                              std::optional<uint32_t> preferred_numa_node,
                              Task task) {
    return impl_ && impl_->enqueue(worker_class, spatial_affinity_key,
                                   preferred_numa_node, std::move(task));
}

bool ClassWorkerPool::enqueue_with_deadline(
    WorkerClass worker_class,
    std::optional<uint64_t> spatial_affinity_key,
    std::optional<uint32_t> preferred_numa_node,
    std::optional<std::chrono::steady_clock::time_point> deadline,
    Task task) {
    return impl_ && impl_->enqueue_with_deadline(
        worker_class, spatial_affinity_key, preferred_numa_node, deadline, std::move(task));
}
void ClassWorkerPool::stop() noexcept { if (impl_) impl_->stop(); }
void ClassWorkerPool::join() noexcept { if (impl_) impl_->join(); }
std::size_t ClassWorkerPool::queued(WorkerClass worker_class) const noexcept {
    return impl_ ? impl_->queued(worker_class) : 0;
}

} // namespace nvlink::broker
