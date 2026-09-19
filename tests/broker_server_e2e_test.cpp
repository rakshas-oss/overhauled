#include "broker_protocol.h"
#include "broker_server.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
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
    if (payload.size() != values.size() * sizeof(double)) {
        throw std::runtime_error("unexpected response payload size");
    }
    std::memcpy(values.data(), payload.data(), payload.size());
    return values;
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool send_request(int socket_fd,
                  const nvlink::broker::BrokerRequest& request,
                  const nvlink::broker::ProtocolLimits& limits) {
    const std::vector<uint8_t> frame = nvlink::broker::encode_request_frame(request, limits);
    const uint32_t len_be = htonl(static_cast<uint32_t>(frame.size()));
    return write_exact(socket_fd, &len_be, sizeof(len_be)) &&
           write_exact(socket_fd, frame.data(), frame.size());
}

bool read_response(int socket_fd,
                   nvlink::broker::BrokerResponse* response,
                   std::string* error,
                   const nvlink::broker::ProtocolLimits& limits) {
    uint32_t response_len_be = 0;
    if (!read_exact(socket_fd, &response_len_be, sizeof(response_len_be))) {
        return false;
    }
    const std::size_t response_len = ntohl(response_len_be);
    if (response_len > limits.max_frame_bytes) {
        return false;
    }
    std::vector<uint8_t> response_frame(response_len);
    if (!read_exact(socket_fd, response_frame.data(), response_frame.size())) {
        return false;
    }
    return nvlink::broker::decode_response_frame(response_frame, response, error, limits);
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
    int sock = -1;
    int truncated_sock = -1;
    auto cleanup = [&] {
        if (truncated_sock >= 0) {
            ::close(truncated_sock);
            truncated_sock = -1;
        }
        if (sock >= 0) {
            ::close(sock);
            sock = -1;
        }
        server.stop();
        if (server_thread.joinable()) {
            server_thread.join();
        }
    };

    try {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        sock = ::socket(AF_INET, SOCK_STREAM, 0);
        require(sock >= 0, "failed to create e2e client socket");

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(config.port));
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0,
                "failed to connect e2e client socket");

        BrokerRequest request;
        request.task_id = "e2e-task";
        request.source = "yukki";
        request.destination = "overhauled";
        request.kind = "inference";
        request.priority = 3;
        request.timeout_ms = 1500;
        request.payload = pack_doubles({5.0, 10.0});

        BrokerResponse response;
        std::string error;
        require(send_request(sock, request, config.service_config.protocol_limits), "failed to send valid request");
        require(read_response(sock, &response, &error, config.service_config.protocol_limits), "failed to read valid response");
        require(response.status == TaskStatus::Ok, "valid request did not return ok status");
        require(response.task_id == request.task_id, "task_id was not preserved for valid response");
        require(response.selected_gpu == -1, "cpu-only mode did not return selected_gpu=-1");

        const auto values = unpack_doubles(response.result);
        require(values[0] == 10.0 && values[1] == 20.0, "unexpected compute output payload");

        std::vector<uint8_t> malformed = encode_request_frame(request, config.service_config.protocol_limits);
        malformed[0] = 0;
        const uint32_t malformed_len_be = htonl(static_cast<uint32_t>(malformed.size()));
        require(write_exact(sock, &malformed_len_be, sizeof(malformed_len_be)), "failed to write malformed frame length");
        require(write_exact(sock, malformed.data(), malformed.size()), "failed to write malformed frame body");
        require(read_response(sock, &response, &error, config.service_config.protocol_limits),
                "failed to read malformed-frame rejection");
        require(response.status == TaskStatus::Rejected, "malformed frame was not rejected");
        require(!response.error.empty(), "malformed frame rejection did not include error");

        request.task_id = "e2e-task-2";
        request.payload = pack_doubles({7.0, 11.0});
        require(send_request(sock, request, config.service_config.protocol_limits), "failed to send second valid request");
        require(read_response(sock, &response, &error, config.service_config.protocol_limits),
                "failed to read second valid response");
        require(response.status == TaskStatus::Ok, "second valid request did not return ok status");
        require(response.task_id == request.task_id, "task_id was not preserved after malformed frame");

        truncated_sock = ::socket(AF_INET, SOCK_STREAM, 0);
        require(truncated_sock >= 0, "failed to create truncated-frame socket");
        require(::connect(truncated_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0,
                "failed to connect truncated-frame socket");
        const std::vector<uint8_t> truncated = encode_request_frame(request, config.service_config.protocol_limits);
        const uint32_t truncated_len_be = htonl(static_cast<uint32_t>(truncated.size()));
        require(write_exact(truncated_sock, &truncated_len_be, sizeof(truncated_len_be)),
                "failed to send truncated-frame length");
        require(write_exact(truncated_sock, truncated.data(), truncated.size() - 1),
                "failed to send truncated-frame partial body");
        ::shutdown(truncated_sock, SHUT_WR);
        BrokerResponse truncated_response;
        std::string truncated_error;
        require(!read_response(truncated_sock, &truncated_response, &truncated_error, config.service_config.protocol_limits),
                "truncated frame unexpectedly produced a decodable response");
        ::close(truncated_sock);
        truncated_sock = -1;

        cleanup();
        std::cout << "broker_server_e2e_test passed\n";
        return 0;
    } catch (const std::exception& ex) {
        cleanup();
        std::cerr << "broker_server_e2e_test failed: " << ex.what() << "\n";
        return 1;
    }
}
