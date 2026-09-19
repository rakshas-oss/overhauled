#include "broker_service.h"

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

std::array<double, 3> unpack_three_doubles(const std::vector<uint8_t>& payload) {
    std::array<double, 3> out{};
    assert(payload.size() == out.size() * sizeof(double));
    std::memcpy(out.data(), payload.data(), payload.size());
    return out;
}

} // namespace

int main() {
    using namespace nvlink::broker;

    BrokerServiceConfig config;
    config.force_cpu_fallback = true;
    config.protocol_limits.max_payload_bytes = 1024;
    BrokerService service(config);

    assert(!service.has_gpu_topology());
    assert(service.gpu_count() == 0);

    BrokerRequest req;
    req.task_id = "task-abc";
    req.source = "yukki";
    req.destination = "overhauled";
    req.kind = "inference";
    req.priority = 5;
    req.timeout_ms = 3000;
    req.payload = pack_doubles({1.0, 2.0, 3.0});

    BrokerResponse ok = service.handle_request(req);
    assert(ok.status == TaskStatus::Ok);
    assert(ok.selected_gpu == -1);
    const auto output = unpack_three_doubles(ok.result);
    assert(output[0] == 2.0);
    assert(output[1] == 4.0);
    assert(output[2] == 6.0);

    req.payload = {0x01, 0x02, 0x03};
    BrokerResponse bad = service.handle_request(req);
    assert(bad.status == TaskStatus::Error);
    assert(!bad.error.empty());

    std::cout << "broker_service_test passed\n";
    return 0;
}
