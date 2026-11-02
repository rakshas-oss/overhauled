// SPDX-License-Identifier: MIT
// Copyright (c) 2024 NVLink Placement Contributors

#include "broker_server.h"
#include "npu_device.h"
#ifdef OVERHAULED_HAS_ETHOSU_DRM
#include "ethosu_u85_drm_executor.h"
#endif

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

std::weak_ptr<nvlink::broker::BrokerServer> g_server;

void signal_handler(int) {
    if (auto server = g_server.lock()) {
        server->stop();
    }
}

void print_usage(const char* argv0) {
    std::cout << "Usage: " << argv0 << " [port] [--cpu-only] [--default-backend cpu|gpu|npu]\n"
              << "       [--gpu-offload-threshold BYTES] [--npu-enabled] [--npu-simulated]\n"
              << "       [--npu-ethosu-bundle FILE] [--ethosu-device /dev/dri/renderD*]\n"
              << "  --npu-ethosu-bundle: execute a Vela-compiled YU85BND1 model via mainline DRM.\n"
              << "  --npu-simulated: enable the CPU-emulated NPU executor for\n"
              << "    conformance/CI only. It is NOT hardware acceleration.\n";
    std::cout << "Starts broker interoperability server for YuKKi-OS task dispatch.\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--help") {
        print_usage(argv[0]);
        return 0;
    }

    nvlink::broker::BrokerServerConfig config;\n    std::string ethosu_bundle;\n    std::string ethosu_device;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--cpu-only") {
            config.service_config.force_cpu_fallback = true;
            continue;
        }
        if (arg == "--default-backend" && i + 1 < argc) {
            const std::string value = argv[++i];
            if (value == "cpu") {
                config.service_config.default_backend = nvlink::broker::BackendKind::Cpu;
            } else if (value == "gpu") {
                config.service_config.default_backend = nvlink::broker::BackendKind::Gpu;
            } else if (value == "npu") {
                config.service_config.default_backend = nvlink::broker::BackendKind::Npu;
            } else {
                std::cerr << "Unknown backend: " << value << "\n";
                return 1;
            }
            continue;
        }
        if (arg == "--gpu-offload-threshold" && i + 1 < argc) {
            config.service_config.gpu_offload_threshold_bytes = std::stoull(argv[++i]);
            continue;
        }
        if (arg == "--npu-enabled") {
            config.service_config.npu_enabled = true;
            continue;
        }
        if (arg == "--npu-ethosu-bundle" && i + 1 < argc) {\n            ethosu_bundle = argv[++i];\n            continue;\n        }\n        if (arg == "--ethosu-device" && i + 1 < argc) {\n            ethosu_device = argv[++i];\n            continue;\n        }\n        if (arg == "--npu-simulated") {
            // Previously --npu-enabled alone wired no executor, so the NPU
            // could never be selected. This flag wires the explicit
            // simulation executor; real hardware executors replace it.
            config.service_config.npu_enabled = true;
            config.service_config.npu_executor =
                std::make_shared<nvlink::broker::SimulatedNpuExecutor>();
            continue;
        }
        config.port = std::stoi(arg);
    }

    try {
        auto server = std::make_shared<nvlink::broker::BrokerServer>(config);
        g_server = server;
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);

        std::cout << "Starting broker server on port " << config.port
                  << (config.service_config.force_cpu_fallback ? " (cpu-only mode)" : "")
                  << "\n";
        server->run();
        std::cout << "Broker server stopped cleanly.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Broker server failed: " << ex.what() << "\n";
        return 1;
    }
}
