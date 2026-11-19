// SPDX-License-Identifier: MIT
#include "worker_scheduler.h"

#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

int main() {
    using namespace nvlink::broker;
    WorkerClass parsed{};
    assert(parse_worker_class("control", &parsed) && parsed == WorkerClass::Control);
    assert(parse_worker_class("realtime-spatial", &parsed) && parsed == WorkerClass::RealtimeSpatial);
    assert(parse_worker_class("throughput", &parsed) && parsed == WorkerClass::Throughput);
    assert(parse_worker_class("exact-cpu", &parsed) && parsed == WorkerClass::ExactCpu);
    assert(parse_worker_class("inference", &parsed) && parsed == WorkerClass::Inference);
    assert(!parse_worker_class("unknown", &parsed));
    assert(std::string(worker_class_name(WorkerClass::RealtimeSpatial)) == "realtime-spatial");

    WorkerPoolConfig config;
    config.workers = {{1, 1, 1, 1, 1}};
    config.max_queue_per_class = 1;
    ClassWorkerPool pool(config);

    std::promise<void> started;
    auto started_future = started.get_future();
    std::promise<void> release;
    auto release_future = release.get_future().share();
    std::promise<WorkerExecutionContext> observed;
    auto observed_future = observed.get_future();

    assert(pool.enqueue(WorkerClass::RealtimeSpatial, 0xabc, std::nullopt, std::nullopt,
        [&started, release_future](const WorkerExecutionContext&) mutable {
            started.set_value();
            release_future.wait();
        }));
    assert(started_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);

    assert(pool.enqueue(WorkerClass::RealtimeSpatial, 0xabc, std::nullopt, std::nullopt,
        [&observed](const WorkerExecutionContext& context) { observed.set_value(context); }));
    assert(pool.queued(WorkerClass::RealtimeSpatial) == 1);
    assert(!pool.enqueue(WorkerClass::RealtimeSpatial, std::nullopt, std::nullopt, std::nullopt,
        [](const WorkerExecutionContext&) {}));

    // A different class has an independent queue and remains admissible.
    std::promise<void> control_done;
    auto control_future = control_done.get_future();
    assert(pool.enqueue(WorkerClass::Control, std::nullopt, std::nullopt, std::nullopt,
        [&control_done](const WorkerExecutionContext& context) {
            assert(context.worker_class == WorkerClass::Control);
            control_done.set_value();
        }));
    assert(control_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);

    release.set_value();
    assert(observed_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    const auto telemetry = observed_future.get();
    assert(telemetry.worker_class == WorkerClass::RealtimeSpatial);
    assert(telemetry.queue_wait_us <= 2'000'000);

    pool.stop();
    assert(!pool.enqueue(WorkerClass::Control, std::nullopt, std::nullopt, std::nullopt,
        [](const WorkerExecutionContext&) {}));
    pool.join();

    // A separate real-time queue orders waiting tasks by earliest deadline.
    WorkerPoolConfig deadline_config;
    deadline_config.workers = {{1, 1, 1, 1, 1}};
    deadline_config.max_queue_per_class = 2;
    ClassWorkerPool deadline_pool(deadline_config);
    std::promise<void> deadline_started;
    auto deadline_started_future = deadline_started.get_future();
    std::promise<void> deadline_release;
    auto deadline_release_future = deadline_release.get_future().share();
    assert(deadline_pool.enqueue(WorkerClass::RealtimeSpatial, 9, std::nullopt, std::nullopt,
        [&deadline_started, deadline_release_future](const WorkerExecutionContext&) {
            deadline_started.set_value();
            deadline_release_future.wait();
        }));
    assert(deadline_started_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);

    std::mutex order_mutex;
    std::vector<int> order;
    std::promise<void> deadlines_done;
    auto deadlines_done_future = deadlines_done.get_future();
    const auto now = std::chrono::steady_clock::now();
    auto record = [&order_mutex, &order, &deadlines_done](int value) {
        std::lock_guard<std::mutex> lock(order_mutex);
        order.push_back(value);
        if (order.size() == 2) deadlines_done.set_value();
    };
    assert(deadline_pool.enqueue(WorkerClass::RealtimeSpatial, 9, std::nullopt,
        now + std::chrono::seconds(60),
        [record](const WorkerExecutionContext&) { record(1); }));
    assert(deadline_pool.enqueue(WorkerClass::RealtimeSpatial, 9, std::nullopt,
        now + std::chrono::seconds(30),
        [record](const WorkerExecutionContext&) { record(2); }));
    deadline_release.set_value();
    assert(deadlines_done_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    assert(order.size() == 2 && order[0] == 2 && order[1] == 1);
    deadline_pool.stop();
    deadline_pool.join();

    std::cout << "worker_scheduler_test passed\n";
    return 0;
}
