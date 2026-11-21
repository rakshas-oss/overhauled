// SPDX-License-Identifier: MIT
// YuKKi-directed class-aware worker pools, nested CPU-group queues and bounded elasticity.

#include "worker_scheduler.h"
#include "worker_topology.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>


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
constexpr std::size_t kMaximumWorkers = kMaximumWorkerThreads;
constexpr std::size_t kMaximumElasticWorkers = kMaximumElasticWorkerThreads;
constexpr std::size_t kElasticWorkerIdBase = kFirstElasticWorkerId;

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


} // namespace

class ClassWorkerPool::Impl {
public:
    explicit Impl(const WorkerPoolConfig& config)
        : config_(config),
          topology_(config.topology_for_testing
              ? *config.topology_for_testing : discover_worker_topology()) {
        std::size_t dedicated_total = 0;
        for (const std::size_t count : config_.workers) {
            if (count == 0)
                throw std::invalid_argument("every worker class needs at least one dedicated worker");
            if (count > kMaximumWorkers || dedicated_total > kMaximumWorkers - count)
                throw std::invalid_argument("dedicated worker total must be in 1..16384");
            dedicated_total += count;
        }
        if (config_.elastic_workers > kMaximumElasticWorkers ||
            config_.elastic_workers > kMaximumWorkers - dedicated_total) {
            throw std::invalid_argument(
                "elastic worker count must fit within the 4096 elastic / 16384 total worker limits");
        }
        for (std::size_t index = 0; index < kWorkerClassCount; ++index) {
            if (config_.elastic_max_per_class[index] > kMaximumElasticWorkers)
                throw std::invalid_argument("per-class elastic worker limit must not exceed 4096");
            const auto value = static_cast<WorkerClass>(index);
            if (!elastic_eligible(value) && config_.elastic_max_per_class[index] != 0)
                throw std::invalid_argument("Control and ExactCpu cannot receive elastic workers");
        }
        if (config_.elastic_workers > 0 &&
            std::all_of(config_.elastic_max_per_class.begin(), config_.elastic_max_per_class.end(),
                        [](std::size_t value) { return value == 0; })) {
            throw std::invalid_argument("elastic workers require at least one eligible class limit");
        }

        for (auto& cursor : node_cursors_) cursor.store(0, std::memory_order_relaxed);
        for (auto& active : elastic_active_) active.store(0, std::memory_order_relaxed);
        for (auto& generation : class_wake_generation_)
            generation.store(0, std::memory_order_relaxed);

        std::string topology_error;
        if (!validate_worker_topology(topology_, &topology_error))
            throw std::invalid_argument("invalid nested CPU topology: " + topology_error);
        if (config_.topology_for_testing) {
            const auto& process_topology = discover_worker_topology();
            if (topology_.allowed_cpus != process_topology.allowed_cpus ||
                topology_.maximum_cpu_id != process_topology.maximum_cpu_id ||
                topology_.affinity_mask_bits != process_topology.affinity_mask_bits ||
                topology_.affinity_supported != process_topology.affinity_supported) {
                throw std::invalid_argument(
                    "synthetic topology must preserve the process-discovered root CPU set and affinity mask");
            }
        }

        nodes_.reserve(topology_.numa_nodes.size());
        for (const auto& numa : topology_.numa_nodes) {
            auto node = std::make_unique<NodeState>();
            node->node_id = numa.id;
            node->groups.reserve(numa.cpu_groups.size());
            for (const auto& cpu_group : numa.cpu_groups) {
                auto group = std::make_unique<CpuGroupState>();
                group->group_id = cpu_group.id;
                node->groups.push_back(std::move(group));
            }
            nodes_.push_back(std::move(node));
        }

        auto domain_for_placement = [this](const WorkerCpuPlacement& placement) -> DomainRef {
            const auto node_it = std::find_if(nodes_.begin(), nodes_.end(),
                [&placement](const std::unique_ptr<NodeState>& node) {
                    return node->node_id == placement.numa_node_id;
                });
            if (node_it == nodes_.end())
                throw std::logic_error("CPU placement escaped its NUMA parent");
            const std::size_t node_index =
                static_cast<std::size_t>(std::distance(nodes_.begin(), node_it));
            const auto& groups = (*node_it)->groups;
            const auto group_it = std::find_if(groups.begin(), groups.end(),
                [&placement](const std::unique_ptr<CpuGroupState>& group) {
                    return group->group_id == placement.cpu_group_id;
                });
            if (group_it == groups.end())
                throw std::logic_error("CPU placement escaped its CPU-group parent");
            return DomainRef{node_index,
                static_cast<std::size_t>(std::distance(groups.begin(), group_it))};
        };

        try {
            // Distribute workers over leaf CPU groups in proportion to the
            // process-allowed CPU count, preserving the machine->NUMA->LLC tree.
            std::size_t worker_ordinal = 0;
            for (std::size_t class_id = 0; class_id < kWorkerClassCount; ++class_id) {
                threads_[class_id].reserve(config_.workers[class_id]);
                for (std::size_t worker = 0; worker < config_.workers[class_id]; ++worker) {
                    const auto placement = worker_cpu_placement(topology_, worker_ordinal++);
                    if (!placement)
                        throw std::logic_error("unable to place dedicated worker in CPU topology");
                    const DomainRef home = domain_for_placement(*placement);
                    ++nodes_[home.node_index]->groups[home.group_index]->dedicated_workers[class_id];
                    threads_[class_id].emplace_back(
                        [this, class_id, worker, home, placement = *placement] {
                            worker_loop(class_id, worker, home, placement);
                        });
                }
            }

            elastic_threads_.reserve(config_.elastic_workers);
            for (std::size_t worker = 0; worker < config_.elastic_workers; ++worker) {
                const auto placement = worker_cpu_placement(topology_, worker_ordinal++);
                if (!placement)
                    throw std::logic_error("unable to place elastic worker in CPU topology");
                const DomainRef home = domain_for_placement(*placement);
                ++nodes_[home.node_index]->groups[home.group_index]->elastic_workers;
                elastic_threads_.emplace_back(
                    [this, worker, home, placement = *placement] {
                        elastic_worker_loop(worker, home, placement);
                    });
            }

            // Only a leaf with a worker capable of draining a class receives
            // that class's queue. This prevents orphaned nested queues.
            for (std::size_t class_id = 0; class_id < kWorkerClassCount; ++class_id) {
                const auto worker_class = static_cast<WorkerClass>(class_id);
                for (std::size_t node_index = 0; node_index < nodes_.size(); ++node_index) {
                    auto& node = *nodes_[node_index];
                    for (std::size_t group_index = 0; group_index < node.groups.size(); ++group_index) {
                        const auto& group = *node.groups[group_index];
                        if (group.dedicated_workers[class_id] > 0 ||
                            (elastic_eligible(worker_class) &&
                             config_.elastic_max_per_class[class_id] > 0 &&
                             group.elastic_workers > 0)) {
                            class_domain_indices_[class_id].push_back(
                                DomainRef{node_index, group_index});
                        }
                    }
                }
                if (class_domain_indices_[class_id].empty())
                    throw std::invalid_argument("worker class has no routable CPU group");
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
        const DomainRef selected = select_domain(index, affinity_key, preferred_node);
        auto& queue = nodes_[selected.node_index]->groups[selected.group_index]->queues[index];
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
        // Separate wake domains let a class worker or an eligible elastic
        // worker steal in hierarchy order without waking unrelated classes.
        signal_class(worker_class, false);
        signal_elastic(false);
        return true;
    }

    void stop() noexcept {
        stopping_.store(true, std::memory_order_release);
        for (std::size_t index = 0; index < kWorkerClassCount; ++index)
            signal_class(static_cast<WorkerClass>(index), true);
        signal_elastic(true);
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
            for (const auto& node : nodes_)
                for (const auto& group : node->groups) {
                    const auto& queue = group->queues[index];
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
                [node_id](const std::unique_ptr<NodeState>& node) {
                    return node->node_id == node_id;
                });
            if (found == nodes_.end()) return 0;
            std::size_t total = 0;
            for (const auto& group : (*found)->groups) {
                const auto& queue = group->queues[index];
                std::lock_guard<std::mutex> lock(queue.mutex);
                total += queue.tasks.size();
            }
            return total;
        } catch (...) {
            return 0;
        }
    }

    std::vector<uint32_t> queue_nodes(WorkerClass worker_class) const {
        const std::size_t index = class_index(worker_class);
        std::vector<uint32_t> result;
        for (const auto& domain : class_domain_indices_[index]) {
            const uint32_t node_id = nodes_[domain.node_index]->node_id;
            if (std::find(result.begin(), result.end(), node_id) == result.end())
                result.push_back(node_id);
        }
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
        std::multiset<QueuedTask, QueuedTaskCompare> tasks;
        uint64_t next_sequence = 0;
    };

    struct CpuGroupState {
        uint32_t group_id = 0;
        std::array<std::size_t, kWorkerClassCount> dedicated_workers{};
        std::size_t elastic_workers = 0;
        std::array<ClassQueue, kWorkerClassCount> queues;
    };

    struct NodeState {
        uint32_t node_id = 0;
        std::vector<std::unique_ptr<CpuGroupState>> groups;
    };

    struct DomainRef {
        std::size_t node_index = 0;
        std::size_t group_index = 0;
    };

    DomainRef select_domain(
        std::size_t class_id,
        std::optional<uint64_t> affinity_key,
        std::optional<uint32_t> preferred_node) {
        const auto& candidates = class_domain_indices_[class_id];
        if (preferred_node) {
            std::size_t matching = 0;
            for (const auto& candidate : candidates)
                if (nodes_[candidate.node_index]->node_id == *preferred_node) ++matching;
            if (matching > 0) {
                const std::size_t target = affinity_key
                    ? static_cast<std::size_t>(*affinity_key % matching)
                    : node_cursors_[class_id].fetch_add(1, std::memory_order_relaxed) % matching;
                std::size_t seen = 0;
                for (const auto& candidate : candidates)
                    if (nodes_[candidate.node_index]->node_id == *preferred_node) {
                        if (seen++ == target) return candidate;
                    }
            }
        }
        if (affinity_key)
            return candidates[static_cast<std::size_t>(*affinity_key % candidates.size())];
        return candidates[node_cursors_[class_id].fetch_add(1, std::memory_order_relaxed)
                          % candidates.size()];
    }

    CpuGroupState& group(const DomainRef& domain) noexcept {
        return *nodes_[domain.node_index]->groups[domain.group_index];
    }

    static bool queue_nonempty(const ClassQueue& queue) {
        std::lock_guard<std::mutex> lock(queue.mutex);
        return !queue.tasks.empty();
    }

    static bool pop_task(ClassQueue& queue, QueuedTask& task) {
        std::lock_guard<std::mutex> lock(queue.mutex);
        if (queue.tasks.empty()) return false;
        auto item = queue.tasks.extract(queue.tasks.begin());
        task = std::move(item.value());
        return true;
    }

    void signal_class(WorkerClass worker_class, bool all) noexcept {
        const std::size_t index = class_index(worker_class);
        class_wake_generation_[index].fetch_add(1, std::memory_order_release);
        if (all) class_wake_cv_[index].notify_all();
        else class_wake_cv_[index].notify_one();
    }

    void signal_elastic(bool all) noexcept {
        elastic_wake_generation_.fetch_add(1, std::memory_order_release);
        if (all) elastic_wake_cv_.notify_all();
        else elastic_wake_cv_.notify_one();
    }

    bool class_has_work(std::size_t class_id) const {
        for (const auto& domain : class_domain_indices_[class_id]) {
            const auto& queue = nodes_[domain.node_index]->groups[domain.group_index]->queues[class_id];
            if (queue_nonempty(queue)) return true;
        }
        return false;
    }

    bool any_elastic_work() const {
        for (std::size_t class_id = 0; class_id < kWorkerClassCount; ++class_id)
            if (config_.elastic_max_per_class[class_id] > 0 && class_has_work(class_id))
                return true;
        return false;
    }

    // Work stealing is strictly hierarchical: own CPU group first, sibling
    // groups in the same NUMA parent second, remote NUMA domains last.
    bool try_take_for_worker(
        std::size_t class_id,
        const DomainRef& home,
        QueuedTask& queued_task) {
        if (pop_task(group(home).queues[class_id], queued_task)) return true;

        const auto& home_node = *nodes_[home.node_index];
        for (std::size_t offset = 1; offset < home_node.groups.size(); ++offset) {
            const std::size_t group_index = (home.group_index + offset) % home_node.groups.size();
            if (pop_task(home_node.groups[group_index]->queues[class_id], queued_task))
                return true;
        }

        for (std::size_t node_offset = 1; node_offset < nodes_.size(); ++node_offset) {
            const std::size_t node_index = (home.node_index + node_offset) % nodes_.size();
            for (const auto& remote_group : nodes_[node_index]->groups)
                if (pop_task(remote_group->queues[class_id], queued_task)) return true;
        }
        return false;
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
        const DomainRef& home,
        std::size_t& schedule_cursor,
        WorkerClass& selected_class,
        QueuedTask& queued_task) {
        for (std::size_t attempt = 0; attempt < kElasticServiceSchedule.size(); ++attempt) {
            const std::size_t slot = (schedule_cursor + attempt) % kElasticServiceSchedule.size();
            const WorkerClass candidate = kElasticServiceSchedule[slot];
            const std::size_t class_id = class_index(candidate);
            if (!class_has_work(class_id) || !reserve_elastic_slot(class_id)) continue;
            if (try_take_for_worker(class_id, home, queued_task)) {
                selected_class = candidate;
                schedule_cursor = (slot + 1) % kElasticServiceSchedule.size();
                return true;
            }
            elastic_active_[class_id].fetch_sub(1, std::memory_order_acq_rel);
            signal_elastic(true);
        }
        return false;
    }

    void execute_task(
        WorkerClass worker_class,
        std::size_t worker_id,
        const WorkerCpuPlacement& placement,
        QueuedTask& queued_task,
        std::optional<std::pair<uint32_t, int>>& current_pin) noexcept {
        const auto started = Clock::now();
        const auto wait = std::chrono::duration_cast<std::chrono::microseconds>(
            started - queued_task.enqueued_at).count();
        WorkerExecutionContext context;
        context.worker_class = worker_class;
        context.worker_id = static_cast<uint32_t>(worker_id);
        context.queue_wait_us = wait > 0 ? static_cast<uint64_t>(wait) : 0;
        if (topology_.affinity_supported &&
            pin_current_thread_to_cpu(placement.cpu_id, topology_.maximum_cpu_id,
                                      topology_.affinity_mask_bits)) {
            current_pin = std::make_pair(placement.numa_node_id,
                                         static_cast<int>(placement.cpu_id));
            context.numa_node = placement.numa_node_id;
            context.cpu_group_id = placement.cpu_group_id;
        } else {
            current_pin.reset();
        }
        try { queued_task.task(context); }
        catch (...) {
            // A task failure is isolated to this worker and its client completion guard.
        }
    }

    void worker_loop(std::size_t class_id,
                     std::size_t worker_index,
                     const DomainRef& home,
                     const WorkerCpuPlacement& placement) noexcept {
        const WorkerClass worker_class = static_cast<WorkerClass>(class_id);
        std::optional<std::pair<uint32_t, int>> current_pin;
        for (;;) {
            const uint64_t observed =
                class_wake_generation_[class_id].load(std::memory_order_acquire);
            QueuedTask queued_task;
            if (try_take_for_worker(class_id, home, queued_task)) {
                if (class_has_work(class_id)) signal_class(worker_class, false);
                execute_task(worker_class,
                    class_id * kMaximumWorkerThreadsPerClass + worker_index,
                    placement, queued_task, current_pin);
                continue;
            }
            if (stopping_.load(std::memory_order_acquire) && !class_has_work(class_id))
                return;
            std::unique_lock<std::mutex> lock(wake_mutex_);
            class_wake_cv_[class_id].wait(lock, [this, class_id, observed] {
                return stopping_.load(std::memory_order_acquire) ||
                       class_wake_generation_[class_id].load(std::memory_order_acquire) != observed;
            });
        }
    }

    void elastic_worker_loop(std::size_t worker_index,
                             const DomainRef& home,
                             const WorkerCpuPlacement& placement) noexcept {
        std::size_t schedule_cursor = worker_index % kElasticServiceSchedule.size();
        std::optional<std::pair<uint32_t, int>> current_pin;
        for (;;) {
            const uint64_t observed =
                elastic_wake_generation_.load(std::memory_order_acquire);
            WorkerClass worker_class = WorkerClass::Throughput;
            QueuedTask queued_task;
            if (try_take_elastic(home, schedule_cursor, worker_class, queued_task)) {
                const std::size_t class_id = class_index(worker_class);
                if (class_has_work(class_id)) signal_elastic(false);
                execute_task(worker_class, kElasticWorkerIdBase + worker_index,
                             placement, queued_task, current_pin);
                elastic_active_[class_id].fetch_sub(1, std::memory_order_acq_rel);
                // Per-class ceilings are process-wide; wake other domains when
                // a slot becomes available so remote backlog cannot be stranded.
                signal_elastic(true);
                continue;
            }
            if (stopping_.load(std::memory_order_acquire) && !any_elastic_work())
                return;
            std::unique_lock<std::mutex> lock(wake_mutex_);
            elastic_wake_cv_.wait(lock, [this, observed] {
                return elastic_wake_generation_.load(std::memory_order_acquire) != observed ||
                       (stopping_.load(std::memory_order_acquire) && !any_elastic_work());
            });
        }
    }

    WorkerPoolConfig config_;
    std::mutex join_mutex_;
    std::mutex wake_mutex_;
    WorkerTopologySnapshot topology_;
    std::vector<std::unique_ptr<NodeState>> nodes_;
    std::array<std::vector<std::thread>, kWorkerClassCount> threads_;
    std::vector<std::thread> elastic_threads_;
    std::array<std::vector<DomainRef>, kWorkerClassCount> class_domain_indices_;
    std::array<std::atomic<std::size_t>, kWorkerClassCount> node_cursors_{};
    std::array<std::atomic<std::size_t>, kWorkerClassCount> elastic_active_{};
    std::array<std::atomic<uint64_t>, kWorkerClassCount> class_wake_generation_{};
    std::array<std::condition_variable, kWorkerClassCount> class_wake_cv_;
    std::atomic<uint64_t> elastic_wake_generation_{0};
    std::condition_variable elastic_wake_cv_;
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