#include "media_stream.h"

#include <limits>

namespace nvlink::media {
namespace {

constexpr std::size_t kHeaderBytes = sizeof(uint32_t) + sizeof(uint16_t);
constexpr std::size_t kFixedFieldsBytes = sizeof(uint8_t) + sizeof(uint16_t) * 2 +
                                         sizeof(uint64_t) * 2 + sizeof(uint32_t) * 4 +
                                         sizeof(uint8_t) + sizeof(uint32_t);

void append_uint(std::vector<uint8_t>* out, uint64_t value, std::size_t bytes) {
    for (std::size_t i = bytes; i > 0; --i) {
        out->push_back(static_cast<uint8_t>(value >> ((i - 1) * 8)));
    }
}

void append_string(std::vector<uint8_t>* out, const std::string& value) {
    append_uint(out, value.size(), sizeof(uint16_t));
    out->insert(out->end(), value.begin(), value.end());
}

bool take(std::size_t need, std::size_t size, std::size_t* offset, std::string* error) {
    if (*offset > size || need > size - *offset) {
        if (error != nullptr) {
            *error = "MED1 frame truncated";
        }
        return false;
    }
    return true;
}

bool read_uint(const std::vector<uint8_t>& bytes,
               std::size_t* offset,
               std::size_t count,
               uint64_t* value,
               std::string* error) {
    if (!take(count, bytes.size(), offset, error)) {
        return false;
    }
    uint64_t result = 0;
    for (std::size_t i = 0; i < count; ++i) {
        result = (result << 8) | bytes[*offset + i];
    }
    *offset += count;
    *value = result;
    return true;
}

bool read_string(const std::vector<uint8_t>& bytes,
                 std::size_t* offset,
                 std::size_t max_string_bytes,
                 std::string* value,
                 std::string* error) {
    uint64_t length = 0;
    if (!read_uint(bytes, offset, sizeof(uint16_t), &length, error)) {
        return false;
    }
    if (length > max_string_bytes) {
        if (error != nullptr) {
            *error = "MED1 string exceeds configured limit";
        }
        return false;
    }
    if (!take(static_cast<std::size_t>(length), bytes.size(), offset, error)) {
        return false;
    }
    value->assign(bytes.begin() + static_cast<std::ptrdiff_t>(*offset),
                  bytes.begin() + static_cast<std::ptrdiff_t>(*offset + length));
    *offset += static_cast<std::size_t>(length);
    return true;
}

} // namespace

bool encode_media_stream_frame(const MediaStreamFrame& frame,
                               std::vector<uint8_t>* out,
                               std::string* error,
                               std::size_t max_payload_bytes,
                               std::size_t max_string_bytes) {
    if (out == nullptr) {
        if (error != nullptr) {
            *error = "output buffer pointer must not be null";
        }
        return false;
    }
    const auto media_type = static_cast<uint8_t>(frame.media_type);
    if (media_type > static_cast<uint8_t>(MediaType::GenericData)) {
        if (error != nullptr) {
            *error = "invalid MED1 media type";
        }
        return false;
    }
    if (frame.codec.size() > max_string_bytes || frame.stream_id.size() > max_string_bytes ||
        frame.codec.size() > std::numeric_limits<uint16_t>::max() ||
        frame.stream_id.size() > std::numeric_limits<uint16_t>::max()) {
        if (error != nullptr) {
            *error = "MED1 string exceeds configured limit";
        }
        return false;
    }
    if (frame.payload.size() > max_payload_bytes ||
        frame.payload.size() > std::numeric_limits<uint32_t>::max()) {
        if (error != nullptr) {
            *error = "MED1 payload exceeds maximum length";
        }
        return false;
    }

    std::vector<uint8_t> encoded;
    encoded.reserve(kHeaderBytes + kFixedFieldsBytes + frame.codec.size() +
                    frame.stream_id.size() + frame.payload.size());
    append_uint(&encoded, MEDIA_MAGIC, sizeof(uint32_t));
    append_uint(&encoded, MEDIA_VERSION, sizeof(uint16_t));
    append_uint(&encoded, media_type, sizeof(uint8_t));
    append_string(&encoded, frame.codec);
    append_string(&encoded, frame.stream_id);
    append_uint(&encoded, frame.sequence_number, sizeof(uint64_t));
    append_uint(&encoded, frame.timestamp_ns, sizeof(uint64_t));
    append_uint(&encoded, frame.sample_rate, sizeof(uint32_t));
    append_uint(&encoded, frame.width, sizeof(uint32_t));
    append_uint(&encoded, frame.height, sizeof(uint32_t));
    append_uint(&encoded, frame.channels, sizeof(uint32_t));
    const uint8_t flags = static_cast<uint8_t>((frame.is_keyframe ? 0x01 : 0) |
                                               (frame.is_final_chunk ? 0x02 : 0));
    append_uint(&encoded, flags, sizeof(uint8_t));
    append_uint(&encoded, frame.payload.size(), sizeof(uint32_t));
    encoded.insert(encoded.end(), frame.payload.begin(), frame.payload.end());
    *out = std::move(encoded);
    return true;
}

bool decode_media_stream_frame(const std::vector<uint8_t>& bytes,
                               MediaStreamFrame* out,
                               std::string* error,
                               std::size_t max_payload_bytes,
                               std::size_t max_string_bytes) {
    if (out == nullptr) {
        if (error != nullptr) {
            *error = "output frame pointer must not be null";
        }
        return false;
    }

    std::size_t offset = 0;
    uint64_t magic = 0;
    uint64_t version = 0;
    if (!read_uint(bytes, &offset, sizeof(uint32_t), &magic, error) ||
        !read_uint(bytes, &offset, sizeof(uint16_t), &version, error)) {
        return false;
    }
    if (magic != MEDIA_MAGIC) {
        if (error != nullptr) {
            *error = "invalid MED1 magic";
        }
        return false;
    }
    if (version != MEDIA_VERSION) {
        if (error != nullptr) {
            *error = "unsupported MED1 version";
        }
        return false;
    }

    MediaStreamFrame frame;
    uint64_t value = 0;
    if (!read_uint(bytes, &offset, sizeof(uint8_t), &value, error)) {
        return false;
    }
    if (value > static_cast<uint8_t>(MediaType::GenericData)) {
        if (error != nullptr) {
            *error = "invalid MED1 media type";
        }
        return false;
    }
    frame.media_type = static_cast<MediaType>(value);
    if (!read_string(bytes, &offset, max_string_bytes, &frame.codec, error) ||
        !read_string(bytes, &offset, max_string_bytes, &frame.stream_id, error)) {
        return false;
    }

    uint64_t* const wide_fields[] = {&frame.sequence_number, &frame.timestamp_ns};
    for (uint64_t* field : wide_fields) {
        if (!read_uint(bytes, &offset, sizeof(uint64_t), field, error)) {
            return false;
        }
    }
    uint32_t* const narrow_fields[] = {
        &frame.sample_rate, &frame.width, &frame.height, &frame.channels,
    };
    for (uint32_t* field : narrow_fields) {
        if (!read_uint(bytes, &offset, sizeof(uint32_t), &value, error)) {
            return false;
        }
        *field = static_cast<uint32_t>(value);
    }

    if (!read_uint(bytes, &offset, sizeof(uint8_t), &value, error)) {
        return false;
    }
    if ((value & ~uint64_t{0x03}) != 0) {
        if (error != nullptr) {
            *error = "invalid MED1 flags";
        }
        return false;
    }
    frame.is_keyframe = (value & 0x01) != 0;
    frame.is_final_chunk = (value & 0x02) != 0;

    if (!read_uint(bytes, &offset, sizeof(uint32_t), &value, error)) {
        return false;
    }
    if (value > max_payload_bytes) {
        if (error != nullptr) {
            *error = "MED1 payload exceeds configured limit";
        }
        return false;
    }
    const std::size_t payload_len = static_cast<std::size_t>(value);
    if (!take(payload_len, bytes.size(), &offset, error)) {
        return false;
    }
    frame.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                         bytes.begin() + static_cast<std::ptrdiff_t>(offset + payload_len));
    offset += payload_len;
    if (offset != bytes.size()) {
        if (error != nullptr) {
            *error = "MED1 frame contains trailing bytes";
        }
        return false;
    }
    *out = std::move(frame);
    return true;
}

} // namespace nvlink::media
