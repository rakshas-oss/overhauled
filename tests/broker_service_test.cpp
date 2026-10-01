#include "broker_service.h"
#include "geospatial_frame.h"

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

    // geospatial.frame.v1 kind: a well-formed NXR1 payload round trips as Ok.
    {
        nvlink::geospatial::GeospatialFrame frame;
        frame.latitude = 10.0;
        frame.longitude = 20.0;
        frame.altitude = 30.0;
        frame.velocity_x = 1.0;
        frame.velocity_y = 2.0;
        frame.velocity_z = 3.0;
        frame.fluidity = 0.5;
        frame.drag = 0.25;
        frame.divergence = 0.125;
        frame.payload = {0xAA, 0xBB};

        std::vector<uint8_t> encoded;
        std::string error;
        const bool encode_ok = nvlink::geospatial::encode_nxr1_frame(frame, &encoded, &error);
        assert(encode_ok);

        BrokerRequest geo_req;
        geo_req.task_id = "task-geo";
        geo_req.source = "yukki";
        geo_req.destination = "overhauled";
        geo_req.kind = nvlink::geospatial::kGeospatialFrameKind;
        geo_req.priority = 1;
        geo_req.timeout_ms = 3000;
        geo_req.payload = encoded;

        BrokerResponse geo_ok = service.handle_request(geo_req);
        assert(geo_ok.status == TaskStatus::Ok);
        assert(geo_ok.error.empty());

        nvlink::geospatial::GeospatialFrame decoded;
        const bool decode_ok = nvlink::geospatial::decode_nxr1_frame(geo_ok.result, &decoded, &error);
        assert(decode_ok);
        assert(decoded.latitude == frame.latitude);
        assert(decoded.payload == frame.payload);

        // A malformed NXR1 payload (bad magic) must be rejected, not crash
        // or silently fall through to the double-array compute path.
        BrokerRequest geo_bad = geo_req;
        geo_bad.payload = encoded;
        geo_bad.payload[0] ^= 0xFF;
        BrokerResponse geo_rejected = service.handle_request(geo_bad);
        assert(geo_rejected.status == TaskStatus::Rejected);
        assert(!geo_rejected.error.empty());
    }

    std::cout << "broker_service_test passed\n";
    return 0;
}
