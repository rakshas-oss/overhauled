#include "broker_protocol.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
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

    std::vector<uint8_t> oversized = encoded_request;
    oversized.resize(limits.max_frame_bytes + 1, 0);
    assert(!decode_request_frame(oversized, &decoded_request, &error, limits));

    std::cout << "broker_protocol_test passed\n";
    return 0;
}
