#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nvlink::geospatial {

/// Magic bytes for the NXR1 geospatial frame wire format ("NXR1").
constexpr uint32_t NXR1_MAGIC = 0x4E585231U;

/// Current NXR1 wire format version.
constexpr uint16_t NXR1_VERSION = 1;

/// Default cap on the opaque trailing payload carried inside an NXR1 frame.
constexpr std::size_t NXR1_DEFAULT_MAX_PAYLOAD_BYTES = 512 * 1024;

/// `BrokerRequest::kind` value that identifies an NXR1-encoded geospatial frame.
constexpr const char* kGeospatialFrameKind = "geospatial.frame.v1";

/// In-memory representation of an NXR1 geospatial frame.
///
/// All numeric fields are transmitted as big-endian IEEE-754 doubles and must
/// be finite (no NaN/Inf) on both encode and decode.
struct GeospatialFrame {
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;
    double velocity_x = 0.0;
    double velocity_y = 0.0;
    double velocity_z = 0.0;
    double fluidity = 0.0;
    double drag = 0.0;
    double divergence = 0.0;
    std::vector<uint8_t> payload;
};

/// Encodes `frame` into the NXR1 wire format.
///
/// Returns false (and sets `*error`) if any numeric field is non-finite or if
/// the payload exceeds `max_payload_bytes`. `out` is left untouched on failure.
bool encode_nxr1_frame(const GeospatialFrame& frame,
                       std::vector<uint8_t>* out,
                       std::string* error,
                       std::size_t max_payload_bytes = NXR1_DEFAULT_MAX_PAYLOAD_BYTES);

/// Decodes `bytes` as an NXR1 frame into `*out`.
///
/// Returns false (and sets `*error`) on truncated input, bad magic/version,
/// a non-finite numeric field, a payload length exceeding
/// `max_payload_bytes`, or trailing bytes after the declared payload.
bool decode_nxr1_frame(const std::vector<uint8_t>& bytes,
                       GeospatialFrame* out,
                       std::string* error,
                       std::size_t max_payload_bytes = NXR1_DEFAULT_MAX_PAYLOAD_BYTES);

} // namespace nvlink::geospatial
