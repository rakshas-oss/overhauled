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
    std::cout
        << "Usage: " << argv0 << " [port] [--cpu-only] [--default-backend cpu|gpu|npu]\n"
        << "       [--gpu-offload-threshold BYTES] [--npu-enabled] [--npu-simulated]\n"
        << "       [--npu-ethosu-bundle FILE] [--ethosu-device /dev/dri/renderD*]\n"
        << "  --npu-ethosu-bundle: execute a Vela-compiled YU85BND1 model via mainline DRM.\n"
        << "  --npu-simulated: enable the CPU-emulated NPU executor for\n"
        << "    conformance/CI only. It is NOT hardware acceleration.\n"
        << "Starts broker interoperability server for YuKKi-OS task dispatch.\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--help") {
        print_usage(argv[0]);
        return 0;
    }

    nvlink::broker::BrokerServerConfig config;
    std::string ethosu_bundle;
    std::string ethosu_device;
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
        if (arg == "--npu-ethosu-bundle" && i + 1 < argc) {
            ethosu_bundle = argv[++i];
            continue;
        }
        if (arg == "--ethosu-device" && i + 1 < argc) {
            ethosu_device = argv[++i];
            continue;
        }
        if (arg == "--npu-simulated") {
            config.service_config.npu_enabled = true;
            config.service_config.npu_executor =
                std::make_shared<nvlink::broker::SimulatedNpuExecutor>();
            continue;
        }
        config.port = std::stoi(arg);
    }

    if (!ethosu_device.empty() && ethosu_bundle.empty()) {
        std::cerr << "--ethosu-device requires --npu-ethosu-bundle FILE\n";
        return 1;
    }
    if (!ethosu_bundle.empty()) {
#ifdef OVERHAULED_HAS_ETHOSU_DRM
        auto executor = std::make_shared<nvlink::broker::EthosU85DrmExecutor>(
            ethosu_bundle, ethosu_device);
        if (!executor->available()) {
            std::cerr << "Ethos-U mainline DRM device unavailable: "
                      << executor->device_path() << "\n";
            return 1;
        }
        config.service_config.npu_enabled = true;
        config.service_config.npu_executor = std::move(executor);
#else
        std::cerr << "Ethos-U DRM support is not compiled in; rebuild with "
                     "-DENABLE_ETHOSU_DRM=ON\n";
        return 1;
#endif
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
