#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nvlink::media {

constexpr uint32_t MEDIA_MAGIC = 0x4D454431U;
constexpr uint16_t MEDIA_VERSION = 1;
constexpr std::size_t MEDIA_DEFAULT_MAX_PAYLOAD_BYTES = 512 * 1024;
constexpr std::size_t MEDIA_DEFAULT_MAX_STRING_BYTES = 1024;
constexpr const char* kMediaStreamKind = "media.stream.v1";

enum class MediaType : uint8_t {
    Video = 0,
    Audio = 1,
    GenericData = 2,
};

struct MediaStreamFrame {
    MediaType media_type = MediaType::GenericData;
    std::string codec;
    std::string stream_id;
    uint64_t sequence_number = 0;
    uint64_t timestamp_ns = 0;
    uint32_t sample_rate = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t channels = 0;
    bool is_keyframe = false;
    bool is_final_chunk = false;
    std::vector<uint8_t> payload;
};

bool encode_media_stream_frame(const MediaStreamFrame& frame,
                               std::vector<uint8_t>* out,
                               std::string* error,
                               std::size_t max_payload_bytes = MEDIA_DEFAULT_MAX_PAYLOAD_BYTES,
                               std::size_t max_string_bytes = MEDIA_DEFAULT_MAX_STRING_BYTES);

bool decode_media_stream_frame(const std::vector<uint8_t>& bytes,
                               MediaStreamFrame* out,
                               std::string* error,
                               std::size_t max_payload_bytes = MEDIA_DEFAULT_MAX_PAYLOAD_BYTES,
                               std::size_t max_string_bytes = MEDIA_DEFAULT_MAX_STRING_BYTES);

} // namespace nvlink::media
