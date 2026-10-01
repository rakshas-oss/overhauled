#include "geospatial_frame.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

namespace {

nvlink::geospatial::GeospatialFrame make_sample_frame() {
    nvlink::geospatial::GeospatialFrame frame;
    frame.latitude = 37.7749;
    frame.longitude = -122.4194;
    frame.altitude = 15.5;
    frame.velocity_x = 1.25;
    frame.velocity_y = -0.5;
    frame.velocity_z = 0.0;
    frame.fluidity = 0.875;
    frame.drag = 0.042;
    frame.divergence = -0.003;
    frame.payload = {0xDE, 0xAD, 0xBE, 0xEF};
    return frame;
}

// NOTE: every call whose return value or output parameter is used by later
// statements is invoked *outside* assert() so the call still executes when
// NDEBUG strips assert() expressions in release builds (see
// broker_protocol_test.cpp for the same convention in this repository).

} // namespace

int main() {
    using namespace nvlink::geospatial;

    // Round trip: encode then decode must reproduce every field exactly.
    {
        const GeospatialFrame frame = make_sample_frame();
        std::vector<uint8_t> encoded;
        std::string error;
        const bool encode_ok = encode_nxr1_frame(frame, &encoded, &error);
        assert(encode_ok);
        assert(!encoded.empty());

        GeospatialFrame decoded;
        const bool decode_ok = decode_nxr1_frame(encoded, &decoded, &error);
        assert(decode_ok);
        assert(decoded.latitude == frame.latitude);
        assert(decoded.longitude == frame.longitude);
        assert(decoded.altitude == frame.altitude);
        assert(decoded.velocity_x == frame.velocity_x);
        assert(decoded.velocity_y == frame.velocity_y);
        assert(decoded.velocity_z == frame.velocity_z);
        assert(decoded.fluidity == frame.fluidity);
        assert(decoded.drag == frame.drag);
        assert(decoded.divergence == frame.divergence);
        assert(decoded.payload == frame.payload);
    }

    // Round trip with an empty payload must also succeed.
    {
        GeospatialFrame frame = make_sample_frame();
        frame.payload.clear();
        std::vector<uint8_t> encoded;
        std::string error;
        const bool encode_ok = encode_nxr1_frame(frame, &encoded, &error);
        assert(encode_ok);
        GeospatialFrame decoded;
        const bool decode_ok = decode_nxr1_frame(encoded, &decoded, &error);
        assert(decode_ok);
        assert(decoded.payload.empty());
    }

    // Truncated frame (missing bytes) must be rejected.
    {
        const GeospatialFrame frame = make_sample_frame();
        std::vector<uint8_t> encoded;
        std::string error;
        const bool encode_ok = encode_nxr1_frame(frame, &encoded, &error);
        assert(encode_ok);
        assert(!encoded.empty());
        encoded.resize(encoded.size() - 1);
        GeospatialFrame decoded;
        const bool decode_ok = decode_nxr1_frame(encoded, &decoded, &error);
        assert(!decode_ok);
        assert(!error.empty());
    }

    // Bad magic must be rejected.
    {
        const GeospatialFrame frame = make_sample_frame();
        std::vector<uint8_t> encoded;
        std::string error;
        const bool encode_ok = encode_nxr1_frame(frame, &encoded, &error);
        assert(encode_ok);
        encoded[0] ^= 0xFF;
        GeospatialFrame decoded;
        const bool decode_ok = decode_nxr1_frame(encoded, &decoded, &error);
        assert(!decode_ok);
        assert(error == "invalid NXR1 magic");
    }

    // Bad version must be rejected.
    {
        const GeospatialFrame frame = make_sample_frame();
        std::vector<uint8_t> encoded;
        std::string error;
        const bool encode_ok = encode_nxr1_frame(frame, &encoded, &error);
        assert(encode_ok);
        encoded[5] = 0x02; // low byte of the big-endian version field
        GeospatialFrame decoded;
        const bool decode_ok = decode_nxr1_frame(encoded, &decoded, &error);
        assert(!decode_ok);
        assert(error == "unsupported NXR1 version");
    }

    // Trailing bytes after a well-formed frame must be rejected.
    {
        const GeospatialFrame frame = make_sample_frame();
        std::vector<uint8_t> encoded;
        std::string error;
        const bool encode_ok = encode_nxr1_frame(frame, &encoded, &error);
        assert(encode_ok);
        encoded.push_back(0x00);
        GeospatialFrame decoded;
        const bool decode_ok = decode_nxr1_frame(encoded, &decoded, &error);
        assert(!decode_ok);
        assert(error == "geospatial frame contains trailing bytes");
    }

    // Oversized payload must be rejected by both encode and decode.
    {
        GeospatialFrame frame = make_sample_frame();
        frame.payload.assign(64, 0x01);
        std::vector<uint8_t> encoded;
        std::string error;
        const bool encode_rejected = !encode_nxr1_frame(frame, &encoded, &error, /*max_payload_bytes=*/32);
        assert(encode_rejected);

        // A declared payload length larger than the configured limit must
        // also be rejected on decode, even if the limit used for encoding
        // was larger.
        const bool encode_ok = encode_nxr1_frame(frame, &encoded, &error);
        assert(encode_ok);
        GeospatialFrame decoded;
        const bool decode_ok = decode_nxr1_frame(encoded, &decoded, &error, /*max_payload_bytes=*/32);
        assert(!decode_ok);
        assert(error == "geospatial frame payload exceeds configured limit");
    }

    // Non-finite numbers (NaN/Inf) must be rejected on encode and decode.
    {
        GeospatialFrame frame = make_sample_frame();
        frame.fluidity = std::numeric_limits<double>::quiet_NaN();
        std::vector<uint8_t> encoded;
        std::string error;
        const bool nan_rejected = !encode_nxr1_frame(frame, &encoded, &error);
        assert(nan_rejected);
        assert(error == "geospatial frame contains a non-finite number");

        frame.fluidity = std::numeric_limits<double>::infinity();
        const bool inf_rejected = !encode_nxr1_frame(frame, &encoded, &error);
        assert(inf_rejected);
        assert(error == "geospatial frame contains a non-finite number");

        // Craft a well-formed frame, then corrupt one double field in-place
        // with an infinity bit pattern to confirm decode rejects it too.
        const GeospatialFrame finite_frame = make_sample_frame();
        const bool encode_ok = encode_nxr1_frame(finite_frame, &encoded, &error);
        assert(encode_ok);
        // Explicit big-endian IEEE-754 +infinity byte layout (sign=0, all
        // exponent bits set, zero mantissa), written independent of host
        // endianness.
        const uint8_t inf_be_bytes[sizeof(double)] = {0x7F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        std::memcpy(encoded.data() + 6, inf_be_bytes, sizeof(inf_be_bytes)); // latitude field
        GeospatialFrame decoded;
        const bool decode_ok = decode_nxr1_frame(encoded, &decoded, &error);
        assert(!decode_ok);
        assert(error == "geospatial frame contains a non-finite number");
    }

    std::cout << "geospatial_frame_test passed\n";
    return 0;
}
