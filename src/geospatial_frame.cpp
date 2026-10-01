#include "geospatial_frame.h"

#include <arpa/inet.h>

#include <cmath>
#include <cstring>
#include <limits>

namespace nvlink::geospatial {
namespace {

// Fixed geospatial/velocity/flow fields transmitted as big-endian doubles,
// in wire order.
constexpr std::size_t kFieldCount = 9;

constexpr std::size_t kHeaderBytes = sizeof(uint32_t) + sizeof(uint16_t);
constexpr std::size_t kFieldsBytes = kFieldCount * sizeof(uint64_t);
constexpr std::size_t kPayloadLenBytes = sizeof(uint32_t);
constexpr std::size_t kFixedFrameBytes = kHeaderBytes + kFieldsBytes + kPayloadLenBytes;

uint64_t host_to_be64(uint64_t value) noexcept {
    const uint64_t high = static_cast<uint64_t>(htonl(static_cast<uint32_t>(value >> 32)));
    const uint64_t low = static_cast<uint64_t>(htonl(static_cast<uint32_t>(value & 0xffffffffULL)));
    return (low << 32) | high;
}

uint64_t be64_to_host(uint64_t value) noexcept {
    const uint64_t high = static_cast<uint64_t>(ntohl(static_cast<uint32_t>(value >> 32)));
    const uint64_t low = static_cast<uint64_t>(ntohl(static_cast<uint32_t>(value & 0xffffffffULL)));
    return (low << 32) | high;
}

void append_bytes(std::vector<uint8_t>* out, const void* data, std::size_t size) {
    const auto* ptr = static_cast<const uint8_t*>(data);
    out->insert(out->end(), ptr, ptr + size);
}

bool append_double(std::vector<uint8_t>* out, double value, std::string* error) {
    if (!std::isfinite(value)) {
        if (error != nullptr) {
            *error = "geospatial frame contains a non-finite number";
        }
        return false;
    }
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint64_t be = host_to_be64(bits);
    append_bytes(out, &be, sizeof(be));
    return true;
}

bool take(std::size_t need, std::size_t size, std::size_t* offset, std::string* error) {
    if (*offset > size || need > size - *offset) {
        if (error != nullptr) {
            *error = "NXR1 frame truncated";
        }
        return false;
    }
    return true;
}

bool read_double(const std::vector<uint8_t>& bytes, std::size_t* offset, double* out, std::string* error) {
    if (!take(sizeof(uint64_t), bytes.size(), offset, error)) {
        return false;
    }
    uint64_t be = 0;
    std::memcpy(&be, bytes.data() + *offset, sizeof(be));
    *offset += sizeof(be);

    const uint64_t bits = be64_to_host(be);
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    if (!std::isfinite(value)) {
        if (error != nullptr) {
            *error = "geospatial frame contains a non-finite number";
        }
        return false;
    }
    *out = value;
    return true;
}

} // namespace

bool encode_nxr1_frame(const GeospatialFrame& frame,
                       std::vector<uint8_t>* out,
                       std::string* error,
                       std::size_t max_payload_bytes) {
    if (out == nullptr) {
        if (error != nullptr) {
            *error = "output buffer pointer must not be null";
        }
        return false;
    }
    if (frame.payload.size() > max_payload_bytes ||
        frame.payload.size() > std::numeric_limits<uint32_t>::max()) {
        if (error != nullptr) {
            *error = "geospatial frame payload exceeds maximum length";
        }
        return false;
    }

    std::vector<uint8_t> frame_bytes;
    frame_bytes.reserve(kFixedFrameBytes + frame.payload.size());

    const uint32_t magic_be = htonl(NXR1_MAGIC);
    const uint16_t version_be = htons(NXR1_VERSION);
    append_bytes(&frame_bytes, &magic_be, sizeof(magic_be));
    append_bytes(&frame_bytes, &version_be, sizeof(version_be));

    const double* const fields[kFieldCount] = {
        &frame.latitude, &frame.longitude,  &frame.altitude, &frame.velocity_x, &frame.velocity_y,
        &frame.velocity_z, &frame.fluidity, &frame.drag,     &frame.divergence,
    };
    for (const double* field : fields) {
        if (!append_double(&frame_bytes, *field, error)) {
            return false;
        }
    }

    const uint32_t payload_len_be = htonl(static_cast<uint32_t>(frame.payload.size()));
    append_bytes(&frame_bytes, &payload_len_be, sizeof(payload_len_be));
    append_bytes(&frame_bytes, frame.payload.data(), frame.payload.size());

    *out = std::move(frame_bytes);
    return true;
}

bool decode_nxr1_frame(const std::vector<uint8_t>& bytes,
                       GeospatialFrame* out,
                       std::string* error,
                       std::size_t max_payload_bytes) {
    if (out == nullptr) {
        if (error != nullptr) {
            *error = "output frame pointer must not be null";
        }
        return false;
    }

    std::size_t offset = 0;
    if (!take(kHeaderBytes, bytes.size(), &offset, error)) {
        return false;
    }
    uint32_t magic_be = 0;
    uint16_t version_be = 0;
    std::memcpy(&magic_be, bytes.data(), sizeof(magic_be));
    std::memcpy(&version_be, bytes.data() + sizeof(magic_be), sizeof(version_be));
    offset = kHeaderBytes;

    if (ntohl(magic_be) != NXR1_MAGIC) {
        if (error != nullptr) {
            *error = "invalid NXR1 magic";
        }
        return false;
    }
    if (ntohs(version_be) != NXR1_VERSION) {
        if (error != nullptr) {
            *error = "unsupported NXR1 version";
        }
        return false;
    }

    GeospatialFrame frame;
    double* const fields[kFieldCount] = {
        &frame.latitude, &frame.longitude,  &frame.altitude, &frame.velocity_x, &frame.velocity_y,
        &frame.velocity_z, &frame.fluidity, &frame.drag,     &frame.divergence,
    };
    for (double* field : fields) {
        if (!read_double(bytes, &offset, field, error)) {
            return false;
        }
    }

    if (!take(kPayloadLenBytes, bytes.size(), &offset, error)) {
        return false;
    }
    uint32_t payload_len_be = 0;
    std::memcpy(&payload_len_be, bytes.data() + offset, sizeof(payload_len_be));
    offset += sizeof(payload_len_be);
    const std::size_t payload_len = ntohl(payload_len_be);

    if (payload_len > max_payload_bytes) {
        if (error != nullptr) {
            *error = "geospatial frame payload exceeds configured limit";
        }
        return false;
    }
    if (!take(payload_len, bytes.size(), &offset, error)) {
        return false;
    }
    frame.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                         bytes.begin() + static_cast<std::ptrdiff_t>(offset + payload_len));
    offset += payload_len;

    if (offset != bytes.size()) {
        if (error != nullptr) {
            *error = "geospatial frame contains trailing bytes";
        }
        return false;
    }

    *out = std::move(frame);
    return true;
}

} // namespace nvlink::geospatial
