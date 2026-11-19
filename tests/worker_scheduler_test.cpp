// SPDX-License-Identifier: MIT
#include "worker_scheduler.h"

#include <array>
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>

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
    std::future<void> started_future = started.get_future();
    std::promise<void> release;
    std::shared_future<void> release_future = release.get_future().share();
    std::promise<WorkerExecutionContext> observed;
    auto observed_future = observed.get_future();

    assert(pool.enqueue(WorkerClass::RealtimeSpatial, 0xabc, std::nullopt, std::nullopt,
        [&started, release_future](const WorkerExecutionContext&) mutable {
            started.set_value();
            release_future.wait();
        }));
    assert(started_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);

    assert(pool.enqueue(WorkerClass::RealtimeSpatial, 0xabc, std::nullopt,
        [&observed](const WorkerExecutionContext& context) {
            observed.set_value(context);
        }));
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
    assert(pool.enqueue(WorkerClass::Control, std::nullopt, std::nullopt,
        [](const WorkerExecutionContext&) {}) == false);
    std::cout << "worker_scheduler_test passed\n";
    return 0;
}
