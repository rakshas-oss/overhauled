#include "broker_protocol.h"
#include "broker_server.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

namespace {

bool read_exact(int socket_fd, void* buffer, std::size_t size) {
    auto* out = static_cast<uint8_t*>(buffer);
    std::size_t total = 0;
    while (total < size) {
        const ssize_t received = ::recv(socket_fd, out + total, size - total, 0);
        if (received <= 0) {
            return false;
        }
        total += static_cast<std::size_t>(received);
    }
    return true;
}

bool write_exact(int socket_fd, const void* buffer, std::size_t size) {
    const auto* in = static_cast<const uint8_t*>(buffer);
    std::size_t total = 0;
    while (total < size) {
        const ssize_t written = ::send(socket_fd, in + total, size - total, 0);
        if (written <= 0) {
            return false;
        }
        total += static_cast<std::size_t>(written);
    }
    return true;
}

std::vector<uint8_t> pack_doubles(const std::array<double, 2>& values) {
    std::vector<uint8_t> payload(values.size() * sizeof(double));
    std::memcpy(payload.data(), values.data(), payload.size());
    return payload;
}

std::array<double, 2> unpack_doubles(const std::vector<uint8_t>& payload) {
    std::array<double, 2> values{};
    assert(payload.size() == values.size() * sizeof(double));
    std::memcpy(values.data(), payload.data(), payload.size());
    return values;
}

} // namespace

int main() {
    using namespace nvlink::broker;

    BrokerServerConfig config;
    config.port = 19090;
    config.service_config.force_cpu_fallback = true;
    config.service_config.protocol_limits.max_frame_bytes = 4096;
    config.service_config.protocol_limits.max_payload_bytes = 1024;

    BrokerServer server(config);
    std::thread server_thread([&server] { server.run(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(sock >= 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(config.port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

    BrokerRequest request;
    request.task_id = "e2e-task";
    request.source = "yukki";
    request.destination = "overhauled";
    request.kind = "inference";
    request.priority = 3;
    request.timeout_ms = 1500;
    request.payload = pack_doubles({5.0, 10.0});

    const std::vector<uint8_t> frame = encode_request_frame(request, config.service_config.protocol_limits);
    const uint32_t len_be = htonl(static_cast<uint32_t>(frame.size()));
    assert(write_exact(sock, &len_be, sizeof(len_be)));
    assert(write_exact(sock, frame.data(), frame.size()));

    uint32_t response_len_be = 0;
    assert(read_exact(sock, &response_len_be, sizeof(response_len_be)));
    const std::size_t response_len = ntohl(response_len_be);
    assert(response_len <= config.service_config.protocol_limits.max_frame_bytes);

    std::vector<uint8_t> response_frame(response_len);
    assert(read_exact(sock, response_frame.data(), response_frame.size()));

    BrokerResponse response;
    std::string error;
    assert(decode_response_frame(response_frame, &response, &error, config.service_config.protocol_limits));
    assert(response.status == TaskStatus::Ok);
    assert(response.task_id == request.task_id);
    assert(response.selected_gpu == -1);

    const auto values = unpack_doubles(response.result);
    assert(values[0] == 10.0);
    assert(values[1] == 20.0);

    ::close(sock);
    server.stop();
    server_thread.join();

    std::cout << "broker_server_e2e_test passed\n";
    return 0;
}
