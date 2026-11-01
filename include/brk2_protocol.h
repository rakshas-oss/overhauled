// SPDX-License-Identifier: MIT
#pragma once

#include "broker_protocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace nvlink::broker {

// BRK2 uses a 32-byte envelope header and 8-byte TLV headers. These helpers
// accept/return the frame body; the TCP uint32_be length prefix is handled by
// BrokerServer exactly once.
constexpr uint32_t BRK2_MAGIC = 0x42524b32U; // "BRK2"
constexpr uint8_t BRK2_MAJOR = 2;
constexpr uint16_t BRK2_HEADER_BYTES = 32;
constexpr uint32_t BRK2_SCHEMA_REQUEST = 1;
constexpr uint32_t BRK2_SCHEMA_RESPONSE = 2;

bool decode_brk2_request_body(const std::vector<uint8_t>& body,
                              BrokerRequest* out,
                              uint64_t* request_id,
                              std::string* error,
                              const ProtocolLimits& limits);

std::vector<uint8_t> encode_brk2_response_body(const BrokerResponse& response,
                                               uint64_t request_id,
                                               const ProtocolLimits& limits);

} // namespace nvlink::broker
