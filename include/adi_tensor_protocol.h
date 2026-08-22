#ifndef ADI_TENSOR_PROTOCOL_H
#define ADI_TENSOR_PROTOCOL_H

#include <cstdint>
#include <vector>
#include <string>

#pragma pack(push, 1)

enum class PacketOpcode : uint8_t {
    VECTOR_TRANSFORM = 0x00,
    LOAD_MODEL       = 0x01,
    EXECUTE_INFERENCE = 0x02,
    UNLOAD_MODEL     = 0x03
};

enum class TensorDataType : uint8_t {
    FP32 = 0x00,
    FP16 = 0x01,
    INT32 = 0x02,
    INT8  = 0x03,
    FP64 = 0x04
};

// Header prefixed on every incoming ADI request
struct RequestHeader {
    uint32_t magic;         // 0x41444931 ("ADI1")
    uint32_t payload_len;   // Payload length in bytes (Big-Endian)
    uint8_t  opcode;        // PacketOpcode
    uint8_t  reserved[3];
};

// Sub-header for Opcode 0x02 (EXECUTE_INFERENCE)
struct InferenceHeader {
    uint32_t model_id;
    uint32_t num_tensors;
    uint32_t batch_size;
};

// Response Header sent back to client
struct ResponseHeader {
    uint64_t timestamp_ns;  // Big-Endian Unix nanoseconds
    uint32_t sequence_id;   // Monotonically increasing sequence ID
    uint8_t  packet_type;   // 0 = Primary, 1 = Delta, 0xFF = Error
    uint8_t  status_code;   // 0 = Success, >0 = Error Code
    uint16_t reserved;
    uint32_t payload_len;   // Payload size in bytes
};

#pragma pack(pop)

// Structure representing a single tensor descriptor
struct TensorDescriptor {
    std::string name;
    TensorDataType dtype;
    std::vector<int64_t> dims;
    size_t byte_size;
};

#endif // ADI_TENSOR_PROTOCOL_H
