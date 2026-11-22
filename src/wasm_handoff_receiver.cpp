// SPDX-License-Identifier: MIT
#include "wasm_handoff_receiver.h"

#include <openssl/evp.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>

namespace nvlink::broker::wasm_handoff {
namespace {
struct MdCtxDeleter { void operator()(EVP_MD_CTX* p) const { EVP_MD_CTX_free(p); } };
struct PkeyDeleter { void operator()(EVP_PKEY* p) const { EVP_PKEY_free(p); } };
using MdCtx = std::unique_ptr<EVP_MD_CTX, MdCtxDeleter>;
using Pkey = std::unique_ptr<EVP_PKEY, PkeyDeleter>;

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
bool digest_bytes(const std::uint8_t* data, std::size_t size, Digest* out) {
    if (!out || (!data && size != 0)) return false;
    MdCtx ctx(EVP_MD_CTX_new());
    unsigned int length = 0;
    return ctx && EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) == 1 &&
        EVP_DigestUpdate(ctx.get(), data, size) == 1 &&
        EVP_DigestFinal_ex(ctx.get(), out->data(), &length) == 1 &&
        length == out->size();
}
} // namespace

std::string sha256_hex(const std::uint8_t* data, std::size_t size) {
    Digest digest{};
    if (!digest_bytes(data, size, &digest)) throw std::runtime_error("OpenSSL SHA-256 failed");
    static constexpr char digits[] = "0123456789abcdef";
    std::string result = "sha256:";
    result.reserve(71);
    for (auto byte : digest) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    return result;
}

bool parse_sha256(const std::string& value, Digest* out) {
    if (!out || value.size() != 71 || value.compare(0, 7, "sha256:") != 0) return false;
    for (std::size_t i = 0; i < out->size(); ++i) {
        const int hi = hex_value(value[7 + i * 2]);
        const int lo = hex_value(value[8 + i * 2]);
        if (hi < 0 || lo < 0) return false;
        (*out)[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return true;
}

bool TrustStore::add(const std::string& key_id, TrustedKey key) {
    if (key_id.empty() || key_id.size() > 128 || key.issuer.empty() ||
        key.subject.empty() || key.audience.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    return keys_.emplace(key_id, std::move(key)).second;
}

bool TrustStore::verify(const std::string& key_id, const std::string& issuer,
                        const std::string& subject, const std::string& audience,
                        const std::uint8_t* message, std::size_t message_size,
                        const Ed25519Signature& signature) const {
    if (!message || message_size == 0 || issuer.empty() || subject.empty() || audience.empty())
        return false;
    TrustedKey trusted;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = keys_.find(key_id);
        if (it == keys_.end()) return false;
        trusted = it->second;
    }
    if (trusted.issuer != issuer || trusted.subject != subject || trusted.audience != audience)
        return false;

    Pkey key(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                         trusted.public_key.data(), trusted.public_key.size()));
    MdCtx ctx(EVP_MD_CTX_new());
    return key && ctx &&
        EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) == 1 &&
        EVP_DigestVerify(ctx.get(), signature.data(), signature.size(),
                         message, message_size) == 1;
}

ReplayGuard::ReplayGuard(std::size_t capacity) : capacity_(capacity) {
    if (capacity_ == 0) throw std::invalid_argument("replay capacity must be positive");
}

bool ReplayGuard::consume(const std::string& sender, const std::string& audience,
                          const std::string& nonce,
                          std::chrono::system_clock::time_point expires_at,
                          std::chrono::system_clock::time_point now) {
    if (sender.empty() || audience.empty() || nonce.size() < 32 || nonce.size() > 128 ||
        expires_at <= now) return false;
    const std::string key = sender + "\n" + audience + "\n" + nonce;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second <= now) it = entries_.erase(it);
        else ++it;
    }
    if (entries_.find(key) != entries_.end() || entries_.size() >= capacity_) return false;
    entries_.emplace(key, expires_at);
    return true;
}

ArtifactChunkReceiver::ArtifactChunkReceiver(std::size_t expected_size,
                                             std::size_t maximum_size,
                                             const std::string& expected_sha256)
    : expected_size_(expected_size) {
    if (expected_size_ == 0 || expected_size_ > maximum_size)
        throw std::invalid_argument("artifact size is zero or exceeds configured maximum");
    if (!parse_sha256(expected_sha256, &expected_digest_))
        throw std::invalid_argument("invalid expected SHA-256 digest");
    bytes_.reserve(expected_size_);
}

bool ArtifactChunkReceiver::append(std::size_t offset, const std::uint8_t* data,
                                   std::size_t size, const std::string& chunk_sha256,
                                   bool final_chunk, std::string* error) {
    auto fail = [&](const char* message) { if (error) *error = message; return false; };
    if (complete_) return fail("artifact transfer is already complete");
    if (!data || size == 0) return fail("empty artifact chunk");
    if (offset != bytes_.size()) return fail("artifact chunk offset is not the next expected byte");
    if (size > expected_size_ - bytes_.size()) return fail("artifact chunk exceeds declared artifact size");
    if (sha256_hex(data, size) != chunk_sha256) return fail("artifact chunk digest mismatch");
    const bool reached_end = size == expected_size_ - bytes_.size();
    if (final_chunk != reached_end) return fail("final-chunk flag does not match declared artifact length");
    if (reached_end) {
        std::vector<std::uint8_t> candidate = bytes_;
        candidate.insert(candidate.end(), data, data + size);
        Digest actual{};
        if (!digest_bytes(candidate.data(), candidate.size(), &actual) ||
            !std::equal(actual.begin(), actual.end(), expected_digest_.begin()))
            return fail("artifact digest mismatch");
        bytes_.swap(candidate);
        complete_ = true;
    } else {
        bytes_.insert(bytes_.end(), data, data + size);
    }
    if (error) error->clear();
    return true;
}

bool ArtifactChunkReceiver::complete() const noexcept { return complete_; }

const std::vector<std::uint8_t>& ArtifactChunkReceiver::bytes() const {
    if (!complete_) throw std::logic_error("artifact is not verified and complete");
    return bytes_;
}

} // namespace nvlink::broker::wasm_handoff
