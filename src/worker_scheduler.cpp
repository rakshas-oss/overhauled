// SPDX-License-Identifier: MIT
// YuKKi-directed class-aware worker pools, NUMA-local queues and bounded elastic capacity.

#include "worker_scheduler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
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
constexpr std::size_t kMaximumWorkers = 256;
constexpr std::size_t kMaximumElasticWorkers = 64;
constexpr std::size_t kElasticWorkerIdBase = kWorkerClassCount * 256;

// Weighted round-robin slots: RealtimeSpatial 3, Throughput 4, Inference 2.
// Control and ExactCpu are intentionally absent from the elastic service set.
constexpr std::array<WorkerClass, 9> kElasticServiceSchedule{{
    WorkerClass::RealtimeSpatial,
    WorkerClass::Throughput,
    WorkerClass::Inference,
    WorkerClass::RealtimeSpatial,
    WorkerClass::Throughput,
    WorkerClass::Throughput,
    WorkerClass::RealtimeSpatial,
    WorkerClass::Throughput,
    WorkerClass::Inference,
}};

std::size_t class_index(WorkerClass value) {
    const auto index = static_cast<std::size_t>(value);
    if (index >= kWorkerClassCount) throw std::invalid_argument("invalid worker class");
    return index;
}

bool elastic_eligible(WorkerClass value) noexcept {
    return value == WorkerClass::RealtimeSpatial ||
           value == WorkerClass::Throughput ||
           value == WorkerClass::Inference;
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
        if (sched_getaffinity(0, sizeof(allowed), &allowed) == 0) {
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
        }
#endif
        // Keep logical node-local queues available on non-Linux systems and
        // restricted containers, but never report this synthetic node as pinned.
        if (nodes_.empty()) nodes_.push_back(CpuNode{0, {}});
    }

    std::size_t node_count() const noexcept { return nodes_.size(); }

    uint32_t node_id(std::size_t index) const {
        if (index >= nodes_.size()) throw std::out_of_range("NUMA node index");
        return nodes_[index].id;
    }

    // Workers remain resident on their home node. Queue routing ensures the
    // task is selected from that node's queue before this affinity is applied.
    std::optional<uint32_t> pin_to_node(
        uint32_t node_id,
        std::size_t worker_index,
        std::optional<std::pair<uint32_t, int>>& current_pin) const noexcept {
#if defined(__linux__)
        const auto selected = std::find_if(nodes_.begin(), nodes_.end(),
            [node_id](const CpuNode& node) { return node.id == node_id; });
        if (selected == nodes_.end() || selected->cpus.empty()) return std::nullopt;
        const int cpu = selected->cpus[worker_index % selected->cpus.size()];
        if (current_pin && current_pin->first == node_id &&
            current_pin->second == cpu) return node_id;
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET(cpu, &mask);
        if (pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask) != 0) {
            current_pin.reset();
            return std::nullopt;
        }
        current_pin = std::make_pair(node_id, cpu);
        return node_id;
#else
        (void)node_id;
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
    explicit Impl(const WorkerPoolConfig& config)
        : config_(config) {
        std::size_t dedicated_total = 0;
        for (const std::size_t count : config_.workers) {
            if (count == 0) throw std::invalid_argument("every worker class needs at least one dedicated worker");
            if (count > kMaximumWorkers || dedicated_total > kMaximumWorkers - count)
                throw std::invalid_argument("dedicated worker total must be in 1..256");
            dedicated_total += count;
        }
        if (config_.elastic_workers > kMaximumElasticWorkers ||
            config_.elastic_workers > kMaximumWorkers - dedicated_total)
            throw std::invalid_argument("elastic worker count must fit within the 64 elastic / 256 total worker limits");
        for (std::size_t index = 0; index < kWorkerClassCount; ++index) {
            if (config_.elastic_max_per_class[index] > kMaximumElasticWorkers)
                throw std::invalid_argument("per-class elastic worker limit must not exceed 64");
            const auto value = static_cast<WorkerClass>(index);
            if (!elastic_eligible(value) && config_.elastic_max_per_class[index] != 0)
                throw std::invalid_argument("Control and ExactCpu cannot receive elastic workers");
        }
        if (config_.elastic_workers > 0 &&
            std::all_of(config_.elastic_max_per_class.begin(), config_.elastic_max_per_class.end(),
                        [](std::size_t value) { return value == 0; }))
            throw std::invalid_argument("elastic workers require at least one eligible class limit");

        for (auto& cursor : node_cursors_) cursor.store(0, std::memory_order_relaxed);
        for (auto& active : elastic_active_) active.store(0, std::memory_order_relaxed);

        const std::size_t nodes = topology_.node_count();
        nodes_.reserve(nodes);
        for (std::size_t index = 0; index < nodes; ++index) {
            auto node = std::make_unique<NodeState>();
            node->node_id = topology_.node_id(index);
            nodes_.push_back(std::move(node));
        }

        try {
            // Dedicated worker counts are spread across visible nodes. Each
            // class has queues only on nodes that can actually drain them.
            for (std::size_t class_id = 0; class_id < kWorkerClassCount; ++class_id) {
                threads_[class_id].reserve(config_.workers[class_id]);
                for (std::size_t worker = 0; worker < config_.workers[class_id]; ++worker) {
                    const std::size_t node_index = (worker + class_id) % nodes_.size();
                    ++nodes_[node_index]->dedicated_workers[class_id];
                    threads_[class_id].emplace_back(
                        [this, class_id, worker, node_index] {
                            worker_loop(class_id, worker, node_index);
                        });
                }
            }

            elastic_threads_.reserve(config_.elastic_workers);
            for (std::size_t worker = 0; worker < config_.elastic_workers; ++worker) {
                const std::size_t node_index = worker % nodes_.size();
                ++nodes_[node_index]->elastic_workers;
                elastic_threads_.emplace_back(
                    [this, worker, node_index] { elastic_worker_loop(worker, node_index); });
            }

            for (std::size_t class_id = 0; class_id < kWorkerClassCount; ++class_id) {
                const auto worker_class = static_cast<WorkerClass>(class_id);
                for (std::size_t node_index = 0; node_index < nodes_.size(); ++node_index) {
                    const auto& node = *nodes_[node_index];
                    if (node.dedicated_workers[class_id] > 0 ||
                        (elastic_eligible(worker_class) &&
                         config_.elastic_max_per_class[class_id] > 0 &&
                         node.elastic_workers > 0)) {
                        class_node_indices_[class_id].push_back(node_index);
                    }
                }
                if (class_node_indices_[class_id].empty())
                    throw std::invalid_argument("worker class has no routable NUMA node");
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
        const std::size_t node_index = select_node(index, affinity_key, preferred_node);
        auto& node = *nodes_[node_index];
        auto& queue = node.queues[index];
        {
            std::lock_guard<std::mutex> lock(queue.mutex);
            if (stopping_.load(std::memory_order_acquire)) return false;
            const auto queue_deadline = worker_class == WorkerClass::RealtimeSpatial
                ? deadline : std::nullopt;
            QueuedTask queued_task{
                Clock::now(), queue_deadline, queue.next_sequence++, std::move(task)};
            try {
                queue.tasks.insert(std::move(queued_task));
            } catch (const std::bad_alloc&) {
                return false;
            }
        }
        queue.cv.notify_one();
        signal_elastic(node, false);
        return true;
    }

    void stop() noexcept {
        stopping_.store(true, std::memory_order_release);
        for (auto& node_ptr : nodes_) {
            NodeState& node = *node_ptr;
            for (auto& queue : node.queues) {
                {
                    std::lock_guard<std::mutex> lock(queue.mutex);
                }
                queue.cv.notify_all();
            }
            signal_elastic(node, true);
        }
    }

    void join() noexcept {
        std::lock_guard<std::mutex> join_lock(join_mutex_);
        for (auto& pool : threads_) {
            for (auto& worker : pool) if (worker.joinable()) worker.join();
            pool.clear();
        }
        for (auto& worker : elastic_threads_) if (worker.joinable()) worker.join();
        elastic_threads_.clear();
    }

    std::size_t queued(WorkerClass worker_class) const noexcept {
        try {
            const std::size_t index = class_index(worker_class);
            std::size_t total = 0;
            for (const auto& node_ptr : nodes_) {
                const auto& queue = node_ptr->queues[index];
                std::lock_guard<std::mutex> lock(queue.mutex);
                total += queue.tasks.size();
            }
            return total;
        } catch (...) {
            return 0;
        }
    }

    std::size_t queued_on_node(WorkerClass worker_class, uint32_t node_id) const noexcept {
        try {
            const std::size_t index = class_index(worker_class);
            const auto found = std::find_if(nodes_.begin(), nodes_.end(),
                [node_id](const std::unique_ptr<NodeState>& node) { return node->node_id == node_id; });
            if (found == nodes_.end()) return 0;
            const auto& queue = (*found)->queues[index];
            std::lock_guard<std::mutex> lock(queue.mutex);
            return queue.tasks.size();
        } catch (...) {
            return 0;
        }
    }

    std::vector<uint32_t> queue_nodes(WorkerClass worker_class) const {
        const std::size_t index = class_index(worker_class);
        std::vector<uint32_t> result;
        result.reserve(class_node_indices_[index].size());
        for (const std::size_t node_index : class_node_indices_[index])
            result.push_back(nodes_[node_index]->node_id);
        return result;
    }

    std::size_t active_elastic(WorkerClass worker_class) const noexcept {
        try {
            return elastic_active_[class_index(worker_class)].load(std::memory_order_acquire);
        } catch (...) {
            return 0;
        }
    }

private:
    struct QueuedTask {
        Clock::time_point enqueued_at{};
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
        std::multiset<QueuedTask, QueuedTaskCompare> tasks;
        uint64_t next_sequence = 0;
    };

    struct NodeState {
        uint32_t node_id = 0;
        std::array<std::size_t, kWorkerClassCount> dedicated_workers{};
        std::size_t elastic_workers = 0;
        std::array<ClassQueue, kWorkerClassCount> queues;
        std::mutex elastic_wake_mutex;
        std::condition_variable elastic_wake_cv;
        uint64_t elastic_generation = 0; // protected by elastic_wake_mutex
    };

    std::size_t select_node(
        std::size_t class_id,
        std::optional<uint64_t> affinity_key,
        std::optional<uint32_t> preferred_node) {
        const auto& candidates = class_node_indices_[class_id];
        if (preferred_node) {
            const auto found = std::find_if(candidates.begin(), candidates.end(),
                [this, preferred_node](std::size_t index) {
                    return nodes_[index]->node_id == *preferred_node;
                });
            if (found != candidates.end()) return *found;
        }
        if (affinity_key)
            return candidates[static_cast<std::size_t>(*affinity_key % candidates.size())];
        return candidates[node_cursors_[class_id].fetch_add(1, std::memory_order_relaxed)
                          % candidates.size()];
    }

    void signal_elastic(NodeState& node, bool all) noexcept {
        {
            std::lock_guard<std::mutex> lock(node.elastic_wake_mutex);
            ++node.elastic_generation;
        }
        if (all) node.elastic_wake_cv.notify_all();
        else node.elastic_wake_cv.notify_one();
    }

    static bool pop_task(ClassQueue& queue, QueuedTask& task) {
        std::lock_guard<std::mutex> lock(queue.mutex);
        if (queue.tasks.empty()) return false;
        auto node = queue.tasks.extract(queue.tasks.begin());
        task = std::move(node.value());
        return true;
    }

    void execute_task(
        WorkerClass worker_class,
        std::size_t worker_id,
        std::size_t worker_index,
        NodeState& node,
        QueuedTask& queued_task,
        bool elastic_worker,
        std::optional<std::pair<uint32_t, int>>& current_pin) noexcept {
        const auto started = Clock::now();
        const auto wait = std::chrono::duration_cast<std::chrono::microseconds>(
            started - queued_task.enqueued_at).count();
        WorkerExecutionContext context;
        context.worker_class = worker_class;
        context.worker_id = static_cast<uint32_t>(worker_id);
        context.queue_wait_us = wait > 0 ? static_cast<uint64_t>(wait) : 0;
        context.numa_node = topology_.pin_to_node(node.node_id, worker_index, current_pin);
        context.elastic_worker = elastic_worker;
        try { queued_task.task(context); }
        catch (...) {
            // A task failure is isolated to this worker and its client completion guard.
        }
    }

    void worker_loop(std::size_t class_id, std::size_t worker_index,
                     std::size_t node_index) noexcept {
        const WorkerClass worker_class = static_cast<WorkerClass>(class_id);
        std::size_t topology_worker_index = worker_index;
        for (std::size_t prior_class = 0; prior_class < class_id; ++prior_class)
            topology_worker_index += config_.workers[prior_class];
        NodeState& node = *nodes_[node_index];
        ClassQueue& queue = node.queues[class_id];
        std::optional<std::pair<uint32_t, int>> current_pin;
        for (;;) {
            QueuedTask queued_task;
            {
                std::unique_lock<std::mutex> lock(queue.mutex);
                queue.cv.wait(lock, [this, &queue] {
                    return stopping_.load(std::memory_order_acquire) || !queue.tasks.empty();
                });
                if (stopping_.load(std::memory_order_acquire) && queue.tasks.empty()) return;
                auto item = queue.tasks.extract(queue.tasks.begin());
                queued_task = std::move(item.value());
            }
            execute_task(worker_class, class_id * 256 + worker_index, topology_worker_index,
                         node, queued_task, false, current_pin);
        }
    }

    bool reserve_elastic_slot(std::size_t class_id) noexcept {
        const std::size_t limit = config_.elastic_max_per_class[class_id];
        if (limit == 0) return false;
        auto& active = elastic_active_[class_id];
        std::size_t current = active.load(std::memory_order_relaxed);
        while (current < limit) {
            if (active.compare_exchange_weak(current, current + 1,
                    std::memory_order_acq_rel, std::memory_order_relaxed)) return true;
        }
        return false;
    }

    bool try_take_elastic(
        NodeState& node,
        std::size_t& schedule_cursor,
        WorkerClass& selected_class,
        QueuedTask& queued_task) {
        for (std::size_t attempt = 0; attempt < kElasticServiceSchedule.size(); ++attempt) {
            const std::size_t slot = (schedule_cursor + attempt) % kElasticServiceSchedule.size();
            const WorkerClass candidate = kElasticServiceSchedule[slot];
            const std::size_t class_id = class_index(candidate);
            if (!reserve_elastic_slot(class_id)) continue;
            if (pop_task(node.queues[class_id], queued_task)) {
                selected_class = candidate;
                schedule_cursor = (slot + 1) % kElasticServiceSchedule.size();
                return true;
            }
            elastic_active_[class_id].fetch_sub(1, std::memory_order_acq_rel);
        }
        return false;
    }

    void elastic_worker_loop(std::size_t worker_index, std::size_t node_index) noexcept {
        NodeState& node = *nodes_[node_index];
        std::size_t schedule_cursor = worker_index % kElasticServiceSchedule.size();
        std::size_t topology_worker_index = 0;
        for (const std::size_t count : config_.workers) topology_worker_index += count;
        topology_worker_index += worker_index;
        std::optional<std::pair<uint32_t, int>> current_pin;
        for (;;) {
            uint64_t observed_generation = 0;
            {
                std::lock_guard<std::mutex> lock(node.elastic_wake_mutex);
                observed_generation = node.elastic_generation;
            }

            WorkerClass worker_class = WorkerClass::Throughput;
            QueuedTask queued_task;
            if (try_take_elastic(node, schedule_cursor, worker_class, queued_task)) {
                const std::size_t class_id = class_index(worker_class);
                execute_task(worker_class, kElasticWorkerIdBase + worker_index,
                             topology_worker_index, node, queued_task,
                             true, current_pin);
                elastic_active_[class_id].fetch_sub(1, std::memory_order_acq_rel);
                // Wake another elastic worker that may have been held back by
                // this class's concurrent-service ceiling.
                signal_elastic(node, false);
                continue;
            }

            if (stopping_.load(std::memory_order_acquire)) return;
            std::unique_lock<std::mutex> lock(node.elastic_wake_mutex);
            node.elastic_wake_cv.wait(lock, [this, &node, observed_generation] {
                return stopping_.load(std::memory_order_acquire) ||
                       node.elastic_generation != observed_generation;
            });
        }
    }

    WorkerPoolConfig config_;
    std::mutex join_mutex_;
    CpuNumaTopology topology_;
    std::vector<std::unique_ptr<NodeState>> nodes_;
    std::array<std::vector<std::thread>, kWorkerClassCount> threads_;
    std::vector<std::thread> elastic_threads_;
    std::array<std::vector<std::size_t>, kWorkerClassCount> class_node_indices_;
    std::array<std::atomic<std::size_t>, kWorkerClassCount> node_cursors_{};
    std::array<std::atomic<std::size_t>, kWorkerClassCount> elastic_active_{};
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
std::size_t ClassWorkerPool::queued_on_node(WorkerClass worker_class, uint32_t node_id) const noexcept {
    return impl_ ? impl_->queued_on_node(worker_class, node_id) : 0;
}
std::vector<uint32_t> ClassWorkerPool::queue_nodes(WorkerClass worker_class) const {
    return impl_ ? impl_->queue_nodes(worker_class) : std::vector<uint32_t>{};
}
std::size_t ClassWorkerPool::active_elastic(WorkerClass worker_class) const noexcept {
    return impl_ ? impl_->active_elastic(worker_class) : 0;
}

} // namespace nvlink::broker