#pragma once

#include "broker_service.h"

#include <atomic>
#include <memory>

namespace nvlink::broker {

struct BrokerServerConfig {
    int port = 9090;
    int socket_timeout_ms = 5000;
    std::size_t max_clients = 128;
    bool verbose = false;
    BrokerServiceConfig service_config{};
};

class BrokerServer {
public:
    explicit BrokerServer(const BrokerServerConfig& config = {});
    ~BrokerServer();

    void run();
    void stop() noexcept;
    bool is_running() const noexcept;

    BrokerServer(const BrokerServer&) = delete;
    BrokerServer& operator=(const BrokerServer&) = delete;

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace nvlink::broker
