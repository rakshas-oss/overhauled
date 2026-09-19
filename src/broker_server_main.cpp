#include "broker_server.h"

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
    std::cout << "Usage: " << argv0 << " [port] [--cpu-only]\n";
    std::cout << "Starts broker interoperability server for YuKKi-OS task dispatch.\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--help") {
        print_usage(argv[0]);
        return 0;
    }

    nvlink::broker::BrokerServerConfig config;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--cpu-only") {
            config.service_config.force_cpu_fallback = true;
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
