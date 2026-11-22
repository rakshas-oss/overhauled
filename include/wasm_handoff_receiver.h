// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace nvlink::broker::wasm_handoff {

using Digest = std::array<std::uint8_t, 32>;
using Ed25519PublicKey = std::array<std::uint8_t, 32>;
using Ed25519Signature = std::array<std::uint8_t, 64>;

std::string sha256_hex(const std::uint8_t* data, std::size_t size);
bool parse_sha256(const std::string& value, Digest* out);

// Trust is provisioned out-of-band. A key is authorized only for the exact
// issuer, subject and audience tuple configured by the operator.
struct TrustedKey {
    Ed25519PublicKey public_key{};
    std::string issuer;
    std::string subject;
    std::string audience;
};

class TrustStore {
public:
    bool add(const std::string& key_id, TrustedKey key);
    bool verify(const std::string& key_id, const std::string& issuer,
                const std::string& subject, const std::string& audience,
                const std::uint8_t* canonical_message, std::size_t message_size,
                const Ed25519Signature& signature) const;
private:
    mutable std::mutex mutex_;
    std::map<std::string, TrustedKey> keys_;
};

// A bounded, process-local replay guard. Call only after signature and
// authorization checks; a durable/distributed deployment must replace this
// with shared replay state before running multiple receiver processes.
class ReplayGuard {
public:
    explicit ReplayGuard(std::size_t capacity = 65536);
    bool consume(const std::string& sender, const std::string& audience,
                 const std::string& nonce,
                 std::chrono::system_clock::time_point expires_at,
                 std::chrono::system_clock::time_point now =
                     std::chrono::system_clock::now());
private:
    std::size_t capacity_;
    std::mutex mutex_;
    std::map<std::string, std::chrono::system_clock::time_point> entries_;
};

// Receives exactly one artifact in strictly increasing, non-overlapping
// chunks. It deliberately rejects retransmission: callers must restart a
// transfer with a new transfer ID rather than risk ambiguous duplicate data.
class ArtifactChunkReceiver {
public:
    ArtifactChunkReceiver(std::size_t expected_size, std::size_t maximum_size,
                          const std::string& expected_sha256);
    bool append(std::size_t offset, const std::uint8_t* bytes,
                std::size_t size, const std::string& chunk_sha256,
                bool final_chunk, std::string* error);
    bool complete() const noexcept;
    const std::vector<std::uint8_t>& bytes() const;
private:
    std::size_t expected_size_;
    Digest expected_digest_{};
    std::vector<std::uint8_t> bytes_;
    bool complete_ = false;
};

} // namespace nvlink::broker::wasm_handoff
