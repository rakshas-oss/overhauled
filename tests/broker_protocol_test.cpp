#include "broker_protocol.h"

#include <arpa/inet.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

std::vector<uint8_t> pack_doubles(const std::array<double, 3>& values) {
    std::vector<uint8_t> payload(values.size() * sizeof(double));
    std::memcpy(payload.data(), values.data(), payload.size());
    return payload;
}

} // namespace

int main() {
    using namespace nvlink::broker;

    ProtocolLimits limits;
    limits.max_frame_bytes = 4096;
    limits.max_payload_bytes = 1024;

    BrokerRequest request;
    request.task_id = "task-123";
    request.source = "yukki";
    request.destination = "overhauled";
    request.kind = "inference";
    request.priority = 7;
    request.timeout_ms = 3000;
    request.payload = pack_doubles({1.0, 2.0, 3.0});

    const std::vector<uint8_t> encoded_request = encode_request_frame(request, limits);
    BrokerRequest decoded_request;
    std::string error;
    assert(decode_request_frame(encoded_request, &decoded_request, &error, limits));
    assert(decoded_request.task_id == request.task_id);
    assert(decoded_request.source == request.source);
    assert(decoded_request.destination == request.destination);
    assert(decoded_request.kind == request.kind);
    assert(decoded_request.priority == request.priority);
    assert(decoded_request.timeout_ms == request.timeout_ms);
    assert(decoded_request.payload == request.payload);

    std::vector<uint8_t> nonzero_reserved_request = encoded_request;
    nonzero_reserved_request[sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint8_t)] = 1;
    assert(!decode_request_frame(nonzero_reserved_request, &decoded_request, &error, limits));
    assert(error == "reserved broker protocol byte must be zero");

    std::string timeout_error;
    BrokerRequest invalid_timeout_request = request;
    invalid_timeout_request.timeout_ms = 0;
    assert(!validate_request(invalid_timeout_request, &timeout_error, limits));
    assert(timeout_error == "timeout_ms must be greater than zero");

    request.timeout_ms = BROKER_MAX_TIMEOUT_MS;
    assert(validate_request(request, &timeout_error, limits));
    const std::vector<uint8_t> max_timeout_frame = encode_request_frame(request, limits);

    invalid_timeout_request = request;
    invalid_timeout_request.timeout_ms = BROKER_MAX_TIMEOUT_MS + 1;
    assert(!validate_request(invalid_timeout_request, &timeout_error, limits));
    assert(timeout_error == "timeout_ms exceeds maximum of 300000 ms");
    bool oversized_encode_rejected = false;
    try {
        (void)encode_request_frame(invalid_timeout_request, limits);
    } catch (const std::runtime_error&) {
        oversized_encode_rejected = true;
    }
    assert(oversized_encode_rejected);

    std::vector<uint8_t> oversized_timeout_frame = max_timeout_frame;
    const std::size_t timeout_offset =
        oversized_timeout_frame.size() - request.payload.size() - 2 * sizeof(uint32_t);
    const uint32_t oversized_timeout_be = htonl(BROKER_MAX_TIMEOUT_MS + 1);
    std::memcpy(oversized_timeout_frame.data() + timeout_offset,
                &oversized_timeout_be,
                sizeof(oversized_timeout_be));
    assert(!decode_request_frame(oversized_timeout_frame, &decoded_request, &timeout_error, limits));
    assert(timeout_error == "timeout_ms exceeds maximum of 300000 ms");

    request.timeout_ms = 3000;
    BrokerResponse response;
    response.task_id = request.task_id;
    response.status = TaskStatus::Ok;
    response.selected_gpu = 1;
    response.latency_ms = 9;
    response.result = pack_doubles({2.0, 4.0, 6.0});

    const std::vector<uint8_t> encoded_response = encode_response_frame(response, limits);
    BrokerResponse decoded_response;
    assert(decode_response_frame(encoded_response, &decoded_response, &error, limits));
    assert(decoded_response.task_id == response.task_id);
    assert(decoded_response.status == response.status);
    assert(decoded_response.selected_gpu == response.selected_gpu);
    assert(decoded_response.latency_ms == response.latency_ms);
    assert(decoded_response.result == response.result);

    std::vector<uint8_t> invalid_status_response = encoded_response;
    const std::size_t status_offset = sizeof(uint32_t) + sizeof(uint16_t) + 2 * sizeof(uint8_t) +
                                      sizeof(uint16_t) + response.task_id.size();
    invalid_status_response[status_offset] = 0xff;
    assert(!decode_response_frame(invalid_status_response, &decoded_response, &error, limits));
    assert(error == "invalid broker response status");

    std::vector<uint8_t> oversized = encoded_request;
    oversized.resize(limits.max_frame_bytes + 1, 0);
    assert(!decode_request_frame(oversized, &decoded_request, &error, limits));

    std::cout << "broker_protocol_test passed\n";
    return 0;
}
