// SPDX-License-Identifier: MIT
// Copyright (c) 2024 NVLink Placement Contributors

#include "broker_service.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::cerr << __FILE__ << ":" << __LINE__ << " CHECK failed: " #cond "\n"; \
            return 1;                                                                \
        }                                                                            \
    } while (0)

using namespace nvlink::broker;

namespace {

std::vector<uint8_t> pack(const std::vector<double>& values) {
    std::vector<uint8_t> payload(values.size() * sizeof(double));
    for (std::size_t i = 0; i < values.size(); ++i) {
        uint64_t bits = 0;
        std::memcpy(&bits, &values[i], sizeof(bits));
        for (std::size_t b = 0; b < sizeof(double); ++b) {
            payload[i * sizeof(double) + b] = static_cast<uint8_t>(bits >> ((7 - b) * 8));
        }
    }
    return payload;
}

double first_value(const std::vector<uint8_t>& payload) {
    uint64_t bits = 0;
    for (std::size_t b = 0; b < sizeof(double); ++b) {
        bits = (bits << 8) | payload[b];
    }
    double v = 0;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

BrokerRequest make_request() {
    BrokerRequest req;
    req.task_id = "t";
    req.source = "yukki";
    req.destination = "overhauled";
    req.kind = "inference";
    req.timeout_ms = 3000;
    req.payload = pack({1.0, 2.0});
    return req;
}

TaskProfile gpu_profile() {
    TaskProfile p;
    p.is_data_parallel = true;
    p.estimated_bytes = 4 * 1024 * 1024;
    return p;
}

struct FakeNpu : NpuExecutor {
    bool available() const override { return true; }
    std::vector<double> run(const std::vector<double>& in) override {
        std::vector<double> out = in;
        for (double& v : out) v *= 3.0;
        return out;
    }
};

BrokerServiceConfig gpu_config() {
    BrokerServiceConfig c;
    c.synthetic_gpu_count = 2;
    return c;
}

} // namespace

int main() {
    // No accelerators present: everything runs on CPU.
    {
        BrokerService svc{BrokerServiceConfig{}};
        BrokerRequest req = make_request();
        req.profile = gpu_profile();
        const auto r = svc.handle_request(req);
        CHECK(r.status == TaskStatus::Ok && r.backend == BackendKind::Cpu);
        CHECK(r.selected_gpu == -1 && first_value(r.result) == 2.0);
    }

    // --cpu-only overrides all metadata, even with GPUs and an NPU.
    {
        BrokerServiceConfig c = gpu_config();
        c.force_cpu_fallback = true;
        c.npu_enabled = true;
        c.npu_executor = std::make_shared<FakeNpu>();
        c.default_backend = BackendKind::Gpu;
        BrokerService svc(c);
        BrokerRequest req = make_request();
        TaskProfile p = gpu_profile();
        p.is_inference = true;
        req.profile = p;
        const auto r = svc.handle_request(req);
        CHECK(r.backend == BackendKind::Cpu && r.selected_gpu == -1);
    }

    // Default (no profile), small and non-parallel tasks choose CPU despite GPUs.
    {
        BrokerService svc(gpu_config());
        BrokerRequest req = make_request();
        CHECK(svc.handle_request(req).backend == BackendKind::Cpu);
        TaskProfile small = gpu_profile();
        small.estimated_bytes = 64;
        req.profile = small;
        CHECK(svc.handle_request(req).backend == BackendKind::Cpu);
        TaskProfile serial;
        serial.estimated_bytes = 1 << 30;
        req.profile = serial;
        CHECK(svc.handle_request(req).backend == BackendKind::Cpu);
    }

    // GPU-suitable work selects GPU and keeps sticky Placer affinity.
    {
        BrokerService svc(gpu_config());
        BrokerRequest req = make_request();
        req.profile = gpu_profile();
        const auto first = svc.handle_request(req);
        CHECK(first.status == TaskStatus::Ok && first.backend == BackendKind::Gpu);
        CHECK(first.selected_gpu >= 0 && first.selected_gpu < 2);
        CHECK(first_value(first.result) == 2.0);
        const auto second = svc.handle_request(req);
        CHECK(second.selected_gpu == first.selected_gpu);
    }

    // Operator default_backend=gpu offloads profile-less requests.
    {
        BrokerServiceConfig c = gpu_config();
        c.default_backend = BackendKind::Gpu;
        BrokerService svc(c);
        CHECK(svc.handle_request(make_request()).backend == BackendKind::Gpu);
    }

    // Exact-precision / verification / crypto work never uses an accelerator.
    {
        BrokerServiceConfig c = gpu_config();
        c.npu_enabled = true;
        c.npu_executor = std::make_shared<FakeNpu>();
        BrokerService svc(c);
        for (int i = 0; i < 3; ++i) {
            TaskProfile p = gpu_profile();
            p.is_inference = true;
            (i == 0 ? p.requires_exact_precision : i == 1 ? p.verification_sensitive : p.cryptographic) = true;
            BrokerRequest req = make_request();
            req.profile = p;
            const auto r = svc.handle_request(req);
            CHECK(r.backend == BackendKind::Cpu && r.selected_gpu == -1);
            // Explicit verified accelerator implementation lifts the restriction.
            p.accelerator_verified = true;
            req.profile = p;
            CHECK(svc.handle_request(req).backend == BackendKind::Npu);
        }
    }

    // NPU requested but unavailable: falls back to CPU, not pretending NPU ran.
    {
        BrokerServiceConfig c;
        c.npu_enabled = true;  // no executor
        BrokerService svc(c);
        BrokerRequest req = make_request();
        TaskProfile p;
        p.is_inference = true;
        req.profile = p;
        CHECK(svc.handle_request(req).backend == BackendKind::Cpu);

        BrokerServiceConfig c2;  // executor present but not enabled
        c2.npu_executor = std::make_shared<FakeNpu>();
        BrokerService svc2(c2);
        CHECK(svc2.handle_request(req).backend == BackendKind::Cpu);

        BrokerServiceConfig c3;
        c3.npu_enabled = true;
        c3.npu_executor = std::make_shared<FakeNpu>();
        BrokerService svc3(c3);
        const auto r = svc3.handle_request(req);
        CHECK(r.backend == BackendKind::Npu && first_value(r.result) == 3.0);
    }

    // GPU failure: CPU fallback only when declared safe.
    {
        BrokerServiceConfig c = gpu_config();
        c.gpu_compute_fn = [](const std::vector<double>&, int) -> std::vector<double> {
            throw std::runtime_error("gpu lost");
        };
        BrokerService svc(c);
        BrokerRequest req = make_request();
        TaskProfile p = gpu_profile();
        req.profile = p;
        const auto unsafe = svc.handle_request(req);
        CHECK(unsafe.status == TaskStatus::Error && unsafe.error == "gpu lost");
        CHECK(unsafe.backend == BackendKind::Gpu && !unsafe.fell_back_to_cpu);

        p.fallback_safe = true;
        req.profile = p;
        const auto safe = svc.handle_request(req);
        CHECK(safe.status == TaskStatus::Ok && safe.backend == BackendKind::Cpu);
        CHECK(safe.fell_back_to_cpu && safe.selected_gpu == -1 && first_value(safe.result) == 2.0);
    }

    std::cout << "backend_dispatch_test passed\n";
    return 0;
}
