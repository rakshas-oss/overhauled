#include "media_stream.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

nvlink::media::MediaStreamFrame make_frame(nvlink::media::MediaType media_type) {
    nvlink::media::MediaStreamFrame frame;
    frame.media_type = media_type;
    frame.codec = media_type == nvlink::media::MediaType::Video
                      ? "h264"
                      : (media_type == nvlink::media::MediaType::Audio ? "pcm_s16le" : "raw");
    frame.stream_id = "camera-1";
    frame.sequence_number = 42;
    frame.timestamp_ns = 123456789;
    frame.sample_rate = 48000;
    frame.width = 1920;
    frame.height = 1080;
    frame.channels = 2;
    frame.is_keyframe = media_type == nvlink::media::MediaType::Video;
    frame.is_final_chunk = media_type == nvlink::media::MediaType::GenericData;
    frame.payload = {0x00, 0x7F, 0x80, 0xFF};
    return frame;
}

} // namespace

int main() {
    using namespace nvlink::media;

    for (MediaType media_type : {MediaType::Video, MediaType::Audio, MediaType::GenericData}) {
        const MediaStreamFrame frame = make_frame(media_type);
        std::vector<uint8_t> encoded;
        std::string error;
        const bool encode_ok = encode_media_stream_frame(frame, &encoded, &error);
        assert(encode_ok);

        MediaStreamFrame decoded;
        const bool decode_ok = decode_media_stream_frame(encoded, &decoded, &error);
        assert(decode_ok);
        assert(decoded.media_type == frame.media_type);
        assert(decoded.codec == frame.codec);
        assert(decoded.stream_id == frame.stream_id);
        assert(decoded.sequence_number == frame.sequence_number);
        assert(decoded.timestamp_ns == frame.timestamp_ns);
        assert(decoded.sample_rate == frame.sample_rate);
        assert(decoded.width == frame.width);
        assert(decoded.height == frame.height);
        assert(decoded.channels == frame.channels);
        assert(decoded.is_keyframe == frame.is_keyframe);
        assert(decoded.is_final_chunk == frame.is_final_chunk);
        assert(decoded.payload == frame.payload);
    }

    const MediaStreamFrame sample = make_frame(MediaType::Video);
    std::vector<uint8_t> encoded;
    std::string error;
    const bool encode_ok = encode_media_stream_frame(sample, &encoded, &error);
    assert(encode_ok);

    {
        std::vector<uint8_t> bad_magic = encoded;
        bad_magic[0] ^= 0xFF;
        MediaStreamFrame decoded;
        const bool decode_ok = decode_media_stream_frame(bad_magic, &decoded, &error);
        assert(!decode_ok);
        assert(error == "invalid MED1 magic");
    }
    {
        std::vector<uint8_t> bad_version = encoded;
        bad_version[5] = 0x02;
        MediaStreamFrame decoded;
        const bool decode_ok = decode_media_stream_frame(bad_version, &decoded, &error);
        assert(!decode_ok);
        assert(error == "unsupported MED1 version");
    }
    {
        std::vector<uint8_t> truncated = encoded;
        truncated.pop_back();
        MediaStreamFrame decoded;
        const bool decode_ok = decode_media_stream_frame(truncated, &decoded, &error);
        assert(!decode_ok);
        assert(error == "MED1 frame truncated");
    }
    {
        std::vector<uint8_t> trailing = encoded;
        trailing.push_back(0x00);
        MediaStreamFrame decoded;
        const bool decode_ok = decode_media_stream_frame(trailing, &decoded, &error);
        assert(!decode_ok);
        assert(error == "MED1 frame contains trailing bytes");
    }
    {
        MediaStreamFrame oversized = sample;
        oversized.payload.assign(8, 0x01);
        std::vector<uint8_t> output;
        const bool encode_rejected = !encode_media_stream_frame(oversized, &output, &error, 4);
        assert(encode_rejected);

        const bool default_encode_ok = encode_media_stream_frame(oversized, &output, &error);
        assert(default_encode_ok);
        MediaStreamFrame decoded;
        const bool decode_ok = decode_media_stream_frame(output, &decoded, &error, 4);
        assert(!decode_ok);
        assert(error == "MED1 payload exceeds configured limit");
    }

    std::cout << "media_stream_test passed\n";
    return 0;
}
