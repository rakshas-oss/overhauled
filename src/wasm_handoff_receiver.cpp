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
    const std::size_t previous_size = bytes_.size();
    bytes_.insert(bytes_.end(), data, data + size);
    if (reached_end) {
        Digest actual{};
        if (!digest_bytes(bytes_.data(), bytes_.size(), &actual) ||
            !std::equal(actual.begin(), actual.end(), expected_digest_.begin())) {
            bytes_.resize(previous_size);
            return fail("artifact digest mismatch");
        }
        complete_ = true;
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


#include <openssl/rand.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <set>
#include <sstream>
#include <utility>

namespace nvlink::broker::wasm_handoff {
namespace {

std::string json_string(const std::string& value) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string out = "\"" ;
    for (unsigned char ch : value) {
        switch (ch) {
            case '"': out += "\""; break;
            case '\\': out += "\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    out += "\\u00";
                    out.push_back(hex[ch >> 4]);
                    out.push_back(hex[ch & 0x0f]);
                } else {
                    out.push_back(static_cast<char>(ch));
                }
        }
    }
    out.push_back('"');
    return out;
}

std::string base64_encode(const std::uint8_t* data, std::size_t size) {
    if (size == 0) return "";
    std::string output(4 * ((size + 2) / 3), '\0');
    const int written = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(&output[0]), data,
        static_cast<int>(size));
    if (written < 0) throw std::runtime_error("base64 encoding failed");
    output.resize(static_cast<std::size_t>(written));
    return output;
}

std::string signature_json(const std::string& key_id,
                           const Ed25519Signature& signature) {
    return "{\"alg\":\"Ed25519\",\"key_id\":" + json_string(key_id) +
           ",\"value\":" + json_string(base64_encode(signature.data(), signature.size())) + "}";
}

std::string sender_json(const SignedMetadata& envelope) {
    return "{\"issuer\":" + json_string(envelope.issuer) +
           ",\"key_id\":" + json_string(envelope.key_id) +
           ",\"subject\":" + json_string(envelope.subject) + "}";
}

std::string envelope_prefix(const SignedMetadata& envelope,
                            const std::string& payload_json) {
    // RFC 8785 key order for this ASCII-only contract envelope.
    return "{\"audience\":" + json_string(envelope.audience) +
           ",\"contract\":\"wasm-handoff/1\",\"expires_at\":" +
           std::to_string(envelope.expires_at) +
           ",\"issued_at\":" + std::to_string(envelope.issued_at) +
           ",\"message_id\":" + json_string(envelope.message_id) +
           ",\"nonce\":" + json_string(envelope.nonce) +
           ",\"payload\":" + payload_json +
           ",\"sender\":" + sender_json(envelope) + "}";
}

bool valid_uuid(const std::string& value) {
    if (value.size() != 36) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') return false;
        } else if (!((value[i] >= '0' && value[i] <= '9') ||
                     (value[i] >= 'a' && value[i] <= 'f') ||
                     (value[i] >= 'A' && value[i] <= 'F'))) {
            return false;
        }
    }
    return true;
}

std::string random_uuid() {
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        throw std::runtime_error("secure UUID generation failed");
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
        out.push_back(hex[bytes[i] >> 4]);
        out.push_back(hex[bytes[i] & 0x0f]);
    }
    return out;
}

std::vector<ModuleDescriptor> sorted_modules(const std::vector<ModuleDescriptor>& modules) {
    auto sorted = modules;
    std::sort(sorted.begin(), sorted.end(),
              [](const ModuleDescriptor& a, const ModuleDescriptor& b) {
                  return a.module_id < b.module_id;
              });
    return sorted;
}

bool safe_identifier(const std::string& value, std::size_t max_size) {
    return !value.empty() && value.size() <= max_size &&
        value.find_first_of("\r\n|") == std::string::npos;
}

std::string rate_limit_identity(const SignedMetadata& envelope) {
    return envelope.issuer + "\n" + envelope.subject + "\n" + envelope.key_id;
}

bool prepare_metadata_cost(const PrepareRequest& request,
                           std::size_t maximum_modules,
                           std::size_t* cost) {
    if (!cost || request.modules.empty() || request.modules.size() > maximum_modules ||
        request.required_capabilities.size() > 256) return false;
    std::size_t total = 1024;
    auto add = [&](std::size_t size) {
        if (size > 1024U * 1024U - total) return false;
        total += size;
        return true;
    };
    if (!add(request.envelope.issuer.size()) ||
        !add(request.envelope.subject.size()) ||
        !add(request.envelope.key_id.size()) ||
        !add(request.envelope.audience.size()) ||
        !add(request.deployment_id.size()) ||
        !add(request.release_id.size()) ||
        !add(request.runtime_policy.size()) ||
        !add(request.artifact_set_digest.size()) ||
        !add(request.manifest_digest.size())) return false;
    for (const auto& module : request.modules) {
        if (!add(module.module_id.size()) || !add(module.version.size()) ||
            !add(module.digest.size()) || !add(module.format.size())) return false;
    }
    for (const auto& capability : request.required_capabilities) {
        if (!add(capability.size())) return false;
    }
    *cost = total;
    return true;
}

bool time_valid(const SignedMetadata& message,
                std::chrono::system_clock::time_point now,
                const HandoffPolicy& policy) {
    if (message.issued_at == 0 || message.expires_at <= message.issued_at ||
        message.expires_at - message.issued_at > policy.maximum_message_lifetime_seconds)
        return false;
    const auto now_seconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());
    if (message.issued_at > now_seconds &&
        message.issued_at - now_seconds > policy.clock_skew_seconds) return false;
    if (message.expires_at < now_seconds &&
        now_seconds - message.expires_at > policy.clock_skew_seconds) return false;
    return true;
}

bool verify_message(const TrustStore& trust, ReplayGuard& replay,
                    const SignedMetadata& envelope,
                    const std::string& canonical,
                    std::chrono::system_clock::time_point now,
                    const HandoffPolicy& policy,
                    std::string* error) {
    auto fail = [&](const char* why) { if (error) *error = why; return false; };
    if (envelope.audience != policy.audience) return fail("wrong audience");
    if (!valid_uuid(envelope.message_id)) return fail("invalid message UUID");
    if (!safe_identifier(envelope.issuer, 256) ||
        !safe_identifier(envelope.subject, 256) ||
        !safe_identifier(envelope.key_id, 128) ||
        envelope.nonce.size() < 32 || envelope.nonce.size() > 128)
        return fail("invalid sender identity or nonce");
    if (!time_valid(envelope, now, policy)) return fail("expired or invalid message lifetime");
    if (!trust.verify(envelope.key_id, envelope.issuer, envelope.subject,
                      envelope.audience,
                      reinterpret_cast<const std::uint8_t*>(canonical.data()),
                      canonical.size(), envelope.signature))
        return fail("sender signature or authorization failed");
    const auto expiry = std::chrono::system_clock::time_point(
        std::chrono::seconds(envelope.expires_at));
    if (!replay.consume(envelope.subject, envelope.audience, envelope.nonce, expiry, now))
        return fail("replayed or exhausted nonce");
    if (error) error->clear();
    return true;
}

std::string attestation_signature_json(const PublisherAttestation& attestation) {
    return "{\"alg\":\"Ed25519\",\"key_id\":" + json_string(attestation.key_id) +
           ",\"value\":" + json_string(base64_encode(attestation.signature.data(),
                                                        attestation.signature.size())) + "}";
}

std::string publisher_object(const PublisherAttestation& p) {
    return "{\"audience\":" + json_string(p.audience) +
           ",\"expires_at\":" + std::to_string(p.expires_at) +
           ",\"issued_at\":" + std::to_string(p.issued_at) +
           ",\"issuer\":" + json_string(p.issuer) +
           ",\"key_id\":" + json_string(p.key_id) +
           ",\"signature\":" + attestation_signature_json(p) +
           ",\"subject\":" + json_string(p.subject) + "}";
}

std::string digest_for_modules(const std::vector<ModuleDescriptor>& modules) {
    std::vector<std::uint8_t> bytes;
    for (const auto& module : sorted_modules(modules)) {
        Digest digest{};
        if (!parse_sha256(module.digest, &digest)) return {};
        bytes.insert(bytes.end(), digest.begin(), digest.end());
    }
    return sha256_hex(bytes.data(), bytes.size());
}

} // namespace

Ed25519Signer::Ed25519Signer(const std::string& key_id, const std::string& issuer,
                             const std::string& subject,
                             const Ed25519PrivateKey& private_key)
    : key_id_(key_id), issuer_(issuer), subject_(subject) {
    if (!safe_identifier(key_id_, 128) || !safe_identifier(issuer_, 256) ||
        !safe_identifier(subject_, 256))
        throw std::invalid_argument("invalid Ed25519 signer identity");
    EVP_PKEY* key = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, nullptr, private_key.data(), private_key.size());
    if (!key) throw std::runtime_error("could not import Ed25519 private key");
    key_ = key;
}

Ed25519Signer::Ed25519Signer(Ed25519Signer&& other) noexcept
    : key_id_(std::move(other.key_id_)), issuer_(std::move(other.issuer_)),
      subject_(std::move(other.subject_)), key_(other.key_) {
    other.key_ = nullptr;
}

Ed25519Signer& Ed25519Signer::operator=(Ed25519Signer&& other) noexcept {
    if (this != &other) {
        if (key_) EVP_PKEY_free(static_cast<EVP_PKEY*>(key_));
        key_id_ = std::move(other.key_id_);
        issuer_ = std::move(other.issuer_);
        subject_ = std::move(other.subject_);
        key_ = other.key_;
        other.key_ = nullptr;
    }
    return *this;
}

Ed25519Signer::~Ed25519Signer() {
    if (key_) EVP_PKEY_free(static_cast<EVP_PKEY*>(key_));
}

const std::string& Ed25519Signer::key_id() const noexcept { return key_id_; }
const std::string& Ed25519Signer::issuer() const noexcept { return issuer_; }
const std::string& Ed25519Signer::subject() const noexcept { return subject_; }

Ed25519Signature Ed25519Signer::sign(const std::uint8_t* message,
                                     std::size_t size) const {
    if (!key_ || !message || size == 0)
        throw std::invalid_argument("invalid Ed25519 signing input");
    Ed25519Signature signature{};
    std::size_t signature_size = signature.size();
    MdCtx ctx(EVP_MD_CTX_new());
    if (!ctx || EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr,
                                   static_cast<EVP_PKEY*>(key_)) != 1 ||
        EVP_DigestSign(ctx.get(), signature.data(), &signature_size,
                       message, size) != 1 || signature_size != signature.size())
        throw std::runtime_error("Ed25519 signing failed");
    return signature;
}

Ed25519PublicKey Ed25519Signer::public_key() const {
    Ed25519PublicKey out{};
    std::size_t size = out.size();
    if (!key_ || EVP_PKEY_get_raw_public_key(static_cast<EVP_PKEY*>(key_),
                                              out.data(), &size) != 1 ||
        size != out.size())
        throw std::runtime_error("could not derive Ed25519 public key");
    return out;
}

std::string canonical_manifest(const std::vector<ModuleDescriptor>& modules) {
    const auto sorted = sorted_modules(modules);
    std::string out = "{\"modules\":[";
    bool first = true;
    for (const auto& module : sorted) {
        if (!first) out.push_back(',');
        first = false;
        out += "{\"digest\":" + json_string(module.digest) +
               ",\"format\":" + json_string(module.format) +
               ",\"module_id\":" + json_string(module.module_id) +
               ",\"size_bytes\":" + std::to_string(module.size_bytes) +
               ",\"version\":" + json_string(module.version) + "}";
    }
    out += "]}";
    return out;
}

std::string canonical_publisher_attestation(const PrepareRequest& request) {
    const auto& p = request.publisher;
    return "{\"artifact_set_digest\":" + json_string(request.artifact_set_digest) +
           ",\"audience\":" + json_string(p.audience) +
           ",\"expires_at\":" + std::to_string(p.expires_at) +
           ",\"issued_at\":" + std::to_string(p.issued_at) +
           ",\"issuer\":" + json_string(p.issuer) +
           ",\"key_id\":" + json_string(p.key_id) +
           ",\"manifest_digest\":" + json_string(request.manifest_digest) +
           ",\"subject\":" + json_string(p.subject) + "}";
}

std::string canonical_prepare_message(const PrepareRequest& request) {
    const auto sorted = sorted_modules(request.modules);
    std::string modules = "[";
    bool first = true;
    for (const auto& module : sorted) {
        if (!first) modules.push_back(',');
        first = false;
        modules += "{\"digest\":" + json_string(module.digest) +
                   ",\"format\":" + json_string(module.format) +
                   ",\"module_id\":" + json_string(module.module_id) +
                   ",\"size_bytes\":" + std::to_string(module.size_bytes) +
                   ",\"version\":" + json_string(module.version) + "}";
    }
    modules.push_back(']');
    std::vector<std::string> capabilities = request.required_capabilities;
    std::sort(capabilities.begin(), capabilities.end());
    std::string caps = "[";
    first = true;
    for (const auto& capability : capabilities) {
        if (!first) caps.push_back(',');
        first = false;
        caps += json_string(capability);
    }
    caps.push_back(']');
    const std::string payload =
        "{\"artifact_set_digest\":" + json_string(request.artifact_set_digest) +
        ",\"deployment_id\":" + json_string(request.deployment_id) +
        ",\"kind\":\"prepare_request\""
        ",\"limits\":{\"fuel\":" + std::to_string(request.requested_fuel) +
        ",\"max_instances\":" + std::to_string(request.requested_max_instances) +
        ",\"memory_bytes\":" + std::to_string(request.requested_memory_bytes) +
        ",\"wall_time_ms\":" + std::to_string(request.requested_wall_time_ms) + "}"
        ",\"manifest_digest\":" + json_string(request.manifest_digest) +
        ",\"modules\":" + modules +
        (request.preferred_backend.empty() ? std::string() :
            ",\"preferred_backend\":" + json_string(request.preferred_backend)) +
        ",\"publisher_provenance\":" + publisher_object(request.publisher) +
        ",\"release_id\":" + json_string(request.release_id) +
        ",\"request_id\":" + json_string(request.request_id) +
        ",\"required_capabilities\":" + caps +
        ",\"runtime_policy\":" + json_string(request.runtime_policy) + "}";
    return envelope_prefix(request.envelope, payload);
}

std::string canonical_chunk_message(const ArtifactChunkMessage& message) {
    const std::string payload =
        "{\"artifact_digest\":" + json_string(message.artifact_digest) +
        ",\"chunk_digest\":" + json_string(message.chunk_digest) +
        ",\"data\":" + json_string(base64_encode(message.data.data(), message.data.size())) +
        ",\"final\":" + std::string(message.final ? "true" : "false") +
        ",\"kind\":\"artifact_chunk\""
        ",\"offset\":" + std::to_string(message.offset) +
        ",\"total_length\":" + std::to_string(message.total_length) +
        ",\"transfer_id\":" + json_string(message.transfer_id) + "}";
    return envelope_prefix(message.envelope, payload);
}

std::string canonical_activation_message(const ActivationRequest& request) {
    const std::string payload =
        "{\"kind\":\"activate_request\","
        "\"request_id\":" + json_string(request.request_id) +
        ",\"transfer_id\":" + json_string(request.transfer_id) + "}";
    return envelope_prefix(request.envelope, payload);
}

std::string canonical_prepare_decision_message(const PrepareDecision& decision) {
    std::vector<std::string> capabilities = decision.accepted_capabilities;
    std::sort(capabilities.begin(), capabilities.end());
    std::string caps = "[";
    bool first = true;
    for (const auto& capability : capabilities) {
        if (!first) caps.push_back(',');
        first = false;
        caps += json_string(capability);
    }
    caps.push_back(']');

    std::string payload;
    if (decision.accepted) {
        payload =
            "{\"accepted_capabilities\":" + caps +
            ",\"abi_profile\":" + json_string(decision.abi_profile) +
            ",\"artifact_transfer_id\":" + json_string(decision.transfer_id) +
            ",\"backend_id\":" + json_string(decision.backend_id) +
            ",\"chunk_size_bytes\":" + std::to_string(decision.chunk_size_bytes) +
            ",\"decision\":\"accepted\""
            ",\"engine\":" + json_string(decision.engine) +
            ",\"engine_version\":" + json_string(decision.engine_version) +
            ",\"kind\":\"prepare_decision\""
            ",\"request_id\":" + json_string(decision.request_id) + "}";
    } else {
        payload =
            "{\"decision\":\"rejected\","
            "\"kind\":\"prepare_decision\","
            "\"rejection_code\":" + json_string(decision.rejection_code) +
            ",\"request_id\":" + json_string(decision.request_id) + "}";
    }
    return envelope_prefix(decision.envelope, payload);
}

namespace {
void sign_prepare_decision(PrepareDecision* decision, const HandoffPolicy& policy,
                           const Ed25519Signer& signer,
                           std::chrono::system_clock::time_point now) {
    if (!decision) throw std::invalid_argument("decision pointer must not be null");
    const auto issued = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());
    decision->envelope.message_id = random_uuid();
    decision->envelope.issued_at = issued;
    decision->envelope.expires_at = issued + policy.maximum_message_lifetime_seconds;
    decision->envelope.audience = policy.receipt_audience;
    decision->envelope.issuer = signer.issuer();
    decision->envelope.subject = signer.subject();
    decision->envelope.key_id = signer.key_id();
    decision->envelope.nonce = random_uuid();
    const std::string bytes = canonical_prepare_decision_message(*decision);
    decision->envelope.signature = signer.sign(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
}
} // namespace

std::string canonical_receipt(const HandoffReceipt& receipt) {
    SignedMetadata envelope;
    envelope.message_id = receipt.message_id;
    envelope.issued_at = receipt.issued_at;
    envelope.expires_at = receipt.expires_at;
    envelope.audience = receipt.audience;
    envelope.issuer = receipt.issuer;
    envelope.subject = receipt.subject;
    envelope.key_id = receipt.signer_key_id;
    envelope.nonce = receipt.nonce;
    const std::string payload =
        "{\"abi_profile\":" + json_string(receipt.abi_profile) +
        ",\"artifact_set_digest\":" + json_string(receipt.artifact_set_digest) +
        ",\"backend_id\":" + json_string(receipt.backend_id) +
        ",\"deployment_id\":" + json_string(receipt.deployment_id) +
        ",\"engine\":" + json_string(receipt.engine) +
        ",\"engine_version\":" + json_string(receipt.engine_version) +
        ",\"kind\":\"execution_receipt\""
        ",\"release_id\":" + json_string(receipt.release_id) +
        ",\"request_id\":" + json_string(receipt.request_id) +
        ",\"state\":" + json_string(receipt.state) + "}";
    return envelope_prefix(envelope, payload);
}

class HandoffReceiver::Impl {
public:
    struct ModuleState {
        ModuleDescriptor descriptor;
        std::unique_ptr<ArtifactChunkReceiver> receiver;
    };
    struct Session {
        PrepareRequest request;
        std::string transfer_id;
        std::vector<ModuleState> modules;
        bool activated = false;
        bool validating = false;
        std::size_t staged_bytes = 0;
    };

    struct SenderBucket {
        double tokens = 0.0;
        std::chrono::steady_clock::time_point updated;
        std::chrono::steady_clock::time_point last_seen;
        double byte_tokens = 0.0;
        std::chrono::steady_clock::time_point byte_updated;
    };

    Impl(const TrustStore& sender, const TrustStore& publisher,
         ReplayGuard& replay, HandoffPolicy policy, const Ed25519Signer& signer)
        : sender_trust(sender), publisher_trust(publisher), replay_guard(replay),
          policy(std::move(policy)), receipt_signer(signer),
          global_tokens(static_cast<double>(this->policy.message_burst)),
          global_updated(std::chrono::steady_clock::now()),
          global_byte_tokens(static_cast<double>(this->policy.ingress_byte_burst)),
          global_byte_updated(std::chrono::steady_clock::now()) {
        if (!std::isfinite(this->policy.message_rate_per_second) ||
            !std::isfinite(this->policy.per_sender_message_rate_per_second) ||
            !std::isfinite(this->policy.ingress_bytes_per_second) ||
            !std::isfinite(this->policy.per_sender_ingress_bytes_per_second) ||
            this->policy.message_rate_per_second <= 0.0 ||
            this->policy.message_rate_per_second > 10000.0 ||
            this->policy.per_sender_message_rate_per_second <= 0.0 ||
            this->policy.per_sender_message_rate_per_second > 1000.0 ||
            this->policy.ingress_bytes_per_second <= 0.0 ||
            this->policy.ingress_bytes_per_second > 1024.0 * 1024.0 * 1024.0 ||
            this->policy.per_sender_ingress_bytes_per_second <= 0.0 ||
            this->policy.per_sender_ingress_bytes_per_second > 256.0 * 1024.0 * 1024.0 ||
            this->policy.ingress_byte_burst == 0 ||
            this->policy.ingress_byte_burst > 64U * 1024U * 1024U ||
            this->policy.per_sender_ingress_byte_burst == 0 ||
            this->policy.per_sender_ingress_byte_burst > 16U * 1024U * 1024U ||
            this->policy.message_burst == 0 ||
            this->policy.message_burst > 10000 ||
            this->policy.per_sender_message_burst == 0 ||
            this->policy.per_sender_message_burst > 1000 ||
            this->policy.maximum_rate_limit_subjects == 0 ||
            this->policy.maximum_rate_limit_subjects > 1024 ||
            this->policy.maximum_active_deployments == 0 ||
            this->policy.maximum_active_deployments > 256 ||
            this->policy.maximum_concurrent_sessions == 0 ||
            this->policy.maximum_concurrent_sessions > 256 ||
            this->policy.maximum_staged_bytes == 0 ||
            this->policy.maximum_staged_bytes > 512U * 1024U * 1024U ||
            this->policy.maximum_modules == 0 ||
            this->policy.maximum_modules > 256 ||
            this->policy.maximum_module_bytes < 8 ||
            this->policy.maximum_module_bytes > 64U * 1024U * 1024U ||
            this->policy.maximum_total_bytes < this->policy.maximum_module_bytes ||
            this->policy.maximum_total_bytes > 256U * 1024U * 1024U ||
            this->policy.maximum_chunk_bytes < 4096 ||
            this->policy.maximum_chunk_bytes > 4U * 1024U * 1024U ||
            this->policy.chunk_size_bytes < 4096 ||
            this->policy.chunk_size_bytes > this->policy.maximum_chunk_bytes ||
            this->policy.maximum_message_lifetime_seconds == 0 ||
            this->policy.maximum_message_lifetime_seconds > 300 ||
            this->policy.audience.empty() || this->policy.receipt_audience.empty() ||
            this->policy.available_capabilities.size() > 256)
            throw std::invalid_argument("invalid handoff flood-control policy");
    }

    bool admit_global_message(std::size_t byte_cost = 0) {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(rate_mutex);
        const double message_elapsed =
            std::chrono::duration<double>(now - global_updated).count();
        global_tokens = std::min<double>(
            static_cast<double>(policy.message_burst),
            global_tokens + message_elapsed * policy.message_rate_per_second);
        global_updated = now;
        const double byte_elapsed =
            std::chrono::duration<double>(now - global_byte_updated).count();
        global_byte_tokens = std::min<double>(
            static_cast<double>(policy.ingress_byte_burst),
            global_byte_tokens + byte_elapsed * policy.ingress_bytes_per_second);
        global_byte_updated = now;
        if (global_tokens < 1.0 || static_cast<double>(byte_cost) > global_byte_tokens)
            return false;
        global_tokens -= 1.0;
        global_byte_tokens -= static_cast<double>(byte_cost);
        return true;
    }

    // Sender buckets are populated only after a valid Ed25519 signature has
    // bound the subject to an operator-provisioned key. Unauthenticated callers
    // therefore cannot exhaust the bounded sender table by inventing names.
    bool admit_sender_message(const SignedMetadata& envelope,
                              std::size_t byte_cost = 0) {
        if (envelope.issuer.empty() || envelope.subject.empty() || envelope.key_id.empty())
            return false;
        const std::string bucket_key = rate_limit_identity(envelope);
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(rate_mutex);
        for (auto it = sender_buckets.begin(); it != sender_buckets.end();) {
            if (now - it->second.last_seen > std::chrono::minutes(5))
                it = sender_buckets.erase(it);
            else
                ++it;
        }
        auto found = sender_buckets.find(bucket_key);
        if (found == sender_buckets.end()) {
            if (sender_buckets.size() >= policy.maximum_rate_limit_subjects)
                return false;
            SenderBucket bucket;
            bucket.tokens = static_cast<double>(policy.per_sender_message_burst);
            bucket.updated = now;
            bucket.last_seen = now;
            bucket.byte_tokens = static_cast<double>(policy.per_sender_ingress_byte_burst);
            bucket.byte_updated = now;
            found = sender_buckets.emplace(bucket_key, bucket).first;
        }
        SenderBucket& bucket = found->second;
        const double message_elapsed =
            std::chrono::duration<double>(now - bucket.updated).count();
        bucket.tokens = std::min<double>(
            static_cast<double>(policy.per_sender_message_burst),
            bucket.tokens + message_elapsed * policy.per_sender_message_rate_per_second);
        bucket.updated = now;
        bucket.last_seen = now;
        const double byte_elapsed =
            std::chrono::duration<double>(now - bucket.byte_updated).count();
        bucket.byte_tokens = std::min<double>(
            static_cast<double>(policy.per_sender_ingress_byte_burst),
            bucket.byte_tokens + byte_elapsed * policy.per_sender_ingress_bytes_per_second);
        bucket.byte_updated = now;
        if (bucket.tokens < 1.0 || static_cast<double>(byte_cost) > bucket.byte_tokens)
            return false;
        bucket.tokens -= 1.0;
        bucket.byte_tokens -= static_cast<double>(byte_cost);
        return true;
    }

    const TrustStore& sender_trust;
    const TrustStore& publisher_trust;
    ReplayGuard& replay_guard;
    HandoffPolicy policy;
    const Ed25519Signer& receipt_signer;
    std::mutex activation_mutex;
    std::mutex rate_mutex;
    std::map<std::string, SenderBucket> sender_buckets;
    double global_tokens;
    std::chrono::steady_clock::time_point global_updated;
    double global_byte_tokens;
    std::chrono::steady_clock::time_point global_byte_updated;
    mutable std::mutex mutex;
    std::map<std::string, Session> sessions;
    std::map<std::string, HandoffReceipt> active;
    std::size_t staged_bytes = 0; // includes reserved memory before allocation
    std::size_t reserved_sessions = 0;
};

HandoffReceiver::HandoffReceiver(const TrustStore& sender_trust,
                                 const TrustStore& publisher_trust,
                                 ReplayGuard& replay_guard,
                                 const HandoffPolicy& policy,
                                 const Ed25519Signer& receipt_signer)
    : pimpl_(new Impl(sender_trust, publisher_trust, replay_guard, policy,
                      receipt_signer)) {}

HandoffReceiver::~HandoffReceiver() = default;

PrepareDecision HandoffReceiver::prepare(const PrepareRequest& request,
                                         std::chrono::system_clock::time_point now) {
    PrepareDecision decision;
    decision.request_id = request.request_id;
    std::size_t metadata_cost = 0;
    if (!prepare_metadata_cost(request, pimpl_->policy.maximum_modules, &metadata_cost))
        return decision;
    const bool global_rate_admitted = pimpl_->admit_global_message(metadata_cost);
    auto reject = [&](const char* code) {
        decision.accepted = false;
        decision.decision = "rejected";
        decision.rejection_code = code;
        if (global_rate_admitted)
            sign_prepare_decision(&decision, pimpl_->policy, pimpl_->receipt_signer, now);
        return decision;
    };
    if (!global_rate_admitted) return reject("policy_limit_exceeded");

    if (!valid_uuid(request.request_id) ||
        !safe_identifier(request.deployment_id, 128) ||
        !safe_identifier(request.release_id, 128))
        return reject("unsupported_format");

    std::string error;
    const std::string canonical = canonical_prepare_message(request);
    if (!verify_message(pimpl_->sender_trust, pimpl_->replay_guard,
                        request.envelope, canonical, now, pimpl_->policy, &error))
        return reject(error == "replayed or exhausted nonce" ? "replay_detected" :
                      error == "expired or invalid message lifetime" ? "expired_request" :
                      "signature_invalid");
    if (!pimpl_->admit_sender_message(request.envelope))
        return reject("policy_limit_exceeded");

    const auto& publisher = request.publisher;
    if (publisher.audience != "wasm-artifact-publisher" ||
        !safe_identifier(publisher.issuer, 256) ||
        !safe_identifier(publisher.subject, 256) ||
        !safe_identifier(publisher.key_id, 128) ||
        publisher.issued_at == 0 || publisher.expires_at <= publisher.issued_at ||
        publisher.expires_at - publisher.issued_at >
            pimpl_->policy.maximum_message_lifetime_seconds)
        return reject("signature_invalid");

    const auto now_seconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());
    if ((publisher.issued_at > now_seconds &&
         publisher.issued_at - now_seconds > pimpl_->policy.clock_skew_seconds) ||
        (publisher.expires_at < now_seconds &&
         now_seconds - publisher.expires_at > pimpl_->policy.clock_skew_seconds))
        return reject("expired_request");
    const std::string provenance = canonical_publisher_attestation(request);
    if (!pimpl_->publisher_trust.verify(
            publisher.key_id, publisher.issuer, publisher.subject, publisher.audience,
            reinterpret_cast<const std::uint8_t*>(provenance.data()), provenance.size(),
            publisher.signature))
        return reject("signature_invalid");

    std::set<std::string> module_ids;
    std::set<std::string> module_digests;
    std::size_t total_bytes = 0;
    for (const auto& module : request.modules) {
        Digest digest{};
        if (!safe_identifier(module.module_id, 128) ||
            !safe_identifier(module.version, 64) ||
            module.format != "wasm-core-1" ||
            module.size_bytes < 8 ||
            module.size_bytes > pimpl_->policy.maximum_module_bytes ||
            !parse_sha256(module.digest, &digest) ||
            !module_ids.insert(module.module_id).second ||
            !module_digests.insert(module.digest).second)
            return reject("unsupported_format");
        if (module.size_bytes > pimpl_->policy.maximum_total_bytes - total_bytes)
            return reject("policy_limit_exceeded");
        total_bytes += static_cast<std::size_t>(module.size_bytes);
    }
    if (total_bytes > pimpl_->policy.maximum_total_bytes)
        return reject("policy_limit_exceeded");

    const std::string manifest = canonical_manifest(request.modules);
    if (sha256_hex(reinterpret_cast<const std::uint8_t*>(manifest.data()), manifest.size()) !=
            request.manifest_digest ||
        digest_for_modules(request.modules) != request.artifact_set_digest)
        return reject("artifact_digest_mismatch");

    std::set<std::string> required;
    for (const auto& capability : request.required_capabilities) {
        if (!safe_identifier(capability, 128) || !required.insert(capability).second)
            return reject("capability_denied");
        if (std::find(pimpl_->policy.available_capabilities.begin(),
                      pimpl_->policy.available_capabilities.end(), capability) ==
            pimpl_->policy.available_capabilities.end())
            return reject("capability_denied");
    }

    if (request.requested_memory_bytes > pimpl_->policy.maximum_memory_bytes ||
        request.requested_fuel > pimpl_->policy.maximum_fuel ||
        request.requested_wall_time_ms == 0 ||
        request.requested_wall_time_ms > pimpl_->policy.maximum_wall_time_ms ||
        request.requested_max_instances == 0 ||
        request.requested_max_instances > pimpl_->policy.maximum_instances)
        return reject("policy_limit_exceeded");

    if ((request.runtime_policy != "auto" && request.runtime_policy != "wasmtime") ||
        !pimpl_->policy.wasmtime_available ||
        !pimpl_->policy.allow_trusted_shared_store_workloads ||
        !pimpl_->policy.module_validator ||
        !pimpl_->policy.activate_release ||
        pimpl_->policy.maximum_concurrent_sessions == 0 ||
        pimpl_->policy.maximum_staged_bytes == 0 ||
        pimpl_->policy.engine != "wasmtime" || pimpl_->policy.engine_version.empty())
        return reject("backend_unavailable");

    PrepareDecision accepted;
    accepted.accepted = true;
    accepted.request_id = request.request_id;
    accepted.decision = "accepted";
    accepted.transfer_id = random_uuid();
    accepted.chunk_size_bytes = std::min(pimpl_->policy.chunk_size_bytes,
                                         pimpl_->policy.maximum_chunk_bytes);
    accepted.backend_id = pimpl_->policy.backend_id;
    accepted.engine = pimpl_->policy.engine;
    accepted.engine_version = pimpl_->policy.engine_version;
    accepted.abi_profile = pimpl_->policy.abi_profile;
    accepted.accepted_capabilities = request.required_capabilities;
    sign_prepare_decision(&accepted, pimpl_->policy, pimpl_->receipt_signer, now);

    // Reserve memory and a session slot before constructing any chunk receiver
    // (whose constructor reserves the full declared artifact size).
    {
        std::lock_guard<std::mutex> lock(pimpl_->mutex);
        const auto now_sec = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());
        for (auto it = pimpl_->sessions.begin(); it != pimpl_->sessions.end();) {
            const auto expires = it->second.request.envelope.expires_at +
                                 pimpl_->policy.clock_skew_seconds;
            if (!it->second.activated && !it->second.validating && expires < now_sec) {
                pimpl_->staged_bytes -= it->second.staged_bytes;
                it = pimpl_->sessions.erase(it);
            } else {
                ++it;
            }
        }
        if (pimpl_->sessions.find(request.request_id) != pimpl_->sessions.end())
            return reject("replay_detected");
        if (pimpl_->active.find(request.deployment_id) == pimpl_->active.end() &&
            pimpl_->active.size() >= pimpl_->policy.maximum_active_deployments)
            return reject("policy_limit_exceeded");
        if (pimpl_->sessions.size() + pimpl_->reserved_sessions >=
                pimpl_->policy.maximum_concurrent_sessions ||
            pimpl_->staged_bytes > pimpl_->policy.maximum_staged_bytes ||
            total_bytes > pimpl_->policy.maximum_staged_bytes - pimpl_->staged_bytes)
            return reject("policy_limit_exceeded");
        pimpl_->staged_bytes += total_bytes;
        ++pimpl_->reserved_sessions;
    }

    Impl::Session session;
    session.request = request;
    session.transfer_id = accepted.transfer_id;
    session.staged_bytes = total_bytes;
    try {
        for (const auto& module : request.modules) {
            Impl::ModuleState state;
            state.descriptor = module;
            state.receiver.reset(new ArtifactChunkReceiver(
                static_cast<std::size_t>(module.size_bytes),
                pimpl_->policy.maximum_module_bytes, module.digest));
            session.modules.push_back(std::move(state));
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(pimpl_->mutex);
        --pimpl_->reserved_sessions;
        pimpl_->staged_bytes -= total_bytes;
        return reject("policy_limit_exceeded");
    }

    {
        std::lock_guard<std::mutex> lock(pimpl_->mutex);
        --pimpl_->reserved_sessions;
        if (!pimpl_->sessions.emplace(request.request_id, std::move(session)).second) {
            pimpl_->staged_bytes -= total_bytes;
            return reject("replay_detected");
        }
    }
    return accepted;
}

bool HandoffReceiver::append_chunk(const ArtifactChunkMessage& chunk,
                                   std::string* error,
                                   std::chrono::system_clock::time_point now) {
    auto fail = [&](const char* why) { if (error) *error = why; return false; };
    if (!pimpl_->admit_global_message(chunk.data.size()))
        return fail("global handoff message or byte-rate limit exceeded");
    const std::string canonical = canonical_chunk_message(chunk);
    if (!verify_message(pimpl_->sender_trust, pimpl_->replay_guard,
                        chunk.envelope, canonical, now, pimpl_->policy, error))
        return false;
    if (!pimpl_->admit_sender_message(chunk.envelope, chunk.data.size()))
        return fail("per-sender handoff message or byte-rate limit exceeded");
    if (!valid_uuid(chunk.transfer_id) || chunk.data.empty() ||
        chunk.data.size() > pimpl_->policy.maximum_chunk_bytes ||
        chunk.total_length == 0 || chunk.total_length > pimpl_->policy.maximum_module_bytes)
        return fail("invalid chunk bounds");

    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    auto session_it = std::find_if(pimpl_->sessions.begin(), pimpl_->sessions.end(),
        [&](const auto& entry) { return entry.second.transfer_id == chunk.transfer_id; });
    if (session_it == pimpl_->sessions.end()) return fail("unknown transfer");
    Impl::Session& session = session_it->second;
    if (session.validating) return fail("activation validation is in progress");
    if (chunk.envelope.issuer != session.request.envelope.issuer ||
        chunk.envelope.subject != session.request.envelope.subject ||
        chunk.envelope.key_id != session.request.envelope.key_id)
        return fail("transfer sender changed");

    auto module_it = std::find_if(session.modules.begin(), session.modules.end(),
        [&](const Impl::ModuleState& module) {
            return module.descriptor.digest == chunk.artifact_digest;
        });
    if (module_it == session.modules.end()) return fail("unknown artifact digest");
    if (chunk.total_length != module_it->descriptor.size_bytes)
        return fail("artifact length mismatch");
    return module_it->receiver->append(
        static_cast<std::size_t>(chunk.offset), chunk.data.data(), chunk.data.size(),
        chunk.chunk_digest, chunk.final, error);
}

bool HandoffReceiver::activate(const ActivationRequest& request,
                               HandoffReceipt* receipt, std::string* error,
                               std::chrono::system_clock::time_point now) {
    auto fail = [&](const char* why) { if (error) *error = why; return false; };
    if (!pimpl_->admit_global_message())
        return fail("global handoff message rate limit exceeded");
    std::unique_lock<std::mutex> activation_lock(pimpl_->activation_mutex);
    if (!receipt || !valid_uuid(request.request_id) || !valid_uuid(request.transfer_id))
        return fail("invalid activation request");
    const std::string canonical = canonical_activation_message(request);
    if (!verify_message(pimpl_->sender_trust, pimpl_->replay_guard,
                        request.envelope, canonical, now, pimpl_->policy, error))
        return false;
    if (!pimpl_->admit_sender_message(request.envelope))
        return fail("per-sender handoff message rate limit exceeded");

    std::vector<VerifiedModuleView> modules;
    std::string deployment_id;
    std::string release_id;
    std::string artifact_digest;
    {
        std::lock_guard<std::mutex> lock(pimpl_->mutex);
        auto it = pimpl_->sessions.find(request.request_id);
        if (it == pimpl_->sessions.end() || it->second.transfer_id != request.transfer_id)
            return fail("unknown or mismatched prepared transfer");
        Impl::Session& session = it->second;
        if (pimpl_->active.find(session.request.deployment_id) == pimpl_->active.end() &&
            pimpl_->active.size() >= pimpl_->policy.maximum_active_deployments) {
            pimpl_->staged_bytes -= session.staged_bytes;
            pimpl_->sessions.erase(it);
            return fail("active deployment limit reached");
        }
        if (request.envelope.issuer != session.request.envelope.issuer ||
            request.envelope.subject != session.request.envelope.subject ||
            request.envelope.key_id != session.request.envelope.key_id)
            return fail("activation sender changed");
        if (session.activated || session.validating)
            return fail("transfer already activated or being validated");
        for (const auto& module : session.modules) {
            if (!module.receiver->complete()) return fail("artifact set is incomplete");
            modules.push_back(VerifiedModuleView{&module.descriptor, &module.receiver->bytes()});
        }
        deployment_id = session.request.deployment_id;
        release_id = session.request.release_id;
        artifact_digest = session.request.artifact_set_digest;
        session.validating = true;
    }

    std::string validation_error;
    bool validated = true;
    try {
        for (const auto& item : modules) {
            if (!pimpl_->policy.module_validator(*item.descriptor, *item.bytes,
                                                  &validation_error)) {
                validated = false;
                break;
            }
        }
    } catch (const std::exception& exception) {
        validation_error = exception.what();
        validated = false;
    } catch (...) {
        validation_error = "module validator threw an unknown exception";
        validated = false;
    }

    HandoffReceipt next;
    if (validated) {
        try {
            const auto issued = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());
            next.request_id = request.request_id;
            next.deployment_id = deployment_id;
            next.release_id = release_id;
            next.state = "activated";
            next.backend_id = pimpl_->policy.backend_id;
            next.engine = pimpl_->policy.engine;
            next.engine_version = pimpl_->policy.engine_version;
            next.abi_profile = pimpl_->policy.abi_profile;
            next.artifact_set_digest = artifact_digest;
            next.issued_at = issued;
            next.expires_at = issued + pimpl_->policy.maximum_message_lifetime_seconds;
            next.message_id = random_uuid();
            next.nonce = random_uuid();
            next.audience = pimpl_->policy.receipt_audience;
            next.issuer = pimpl_->receipt_signer.issuer();
            next.subject = pimpl_->receipt_signer.subject();
            next.signer_key_id = pimpl_->receipt_signer.key_id();
            const std::string receipt_bytes = canonical_receipt(next);
            next.signature = pimpl_->receipt_signer.sign(
                reinterpret_cast<const std::uint8_t*>(receipt_bytes.data()),
                receipt_bytes.size());
        } catch (const std::exception& exception) {
            validation_error = exception.what();
            validated = false;
        } catch (...) {
            validation_error = "receipt signing failed";
            validated = false;
        }
    }

    // The embedding runtime owns executable state. This callback must stage
    // all verified modules and atomically switch the active release only after
    // its own compilation/link checks succeed. A false return must leave the
    // previous release untouched.
    if (validated) {
        try {
            validated = pimpl_->policy.activate_release(
                deployment_id, release_id, modules, &validation_error);
        } catch (const std::exception& exception) {
            validation_error = exception.what();
            validated = false;
        } catch (...) {
            validation_error = "runtime activation callback threw an unknown exception";
            validated = false;
        }
    }

    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    auto it = pimpl_->sessions.find(request.request_id);
    if (it == pimpl_->sessions.end() || it->second.transfer_id != request.transfer_id)
        return fail("prepared transfer disappeared during activation");
    Impl::Session& session = it->second;
    if (!validated) {
        pimpl_->staged_bytes -= session.staged_bytes;
        pimpl_->sessions.erase(it);
        if (error) *error = validation_error.empty() ?
            "WASM validation or atomic runtime activation failed" : validation_error;
        return false;
    }
    if (!session.validating || session.activated)
        return fail("prepared transfer state changed during activation");

    // Release the previous active artifact set only after the runtime manager
    // has confirmed the new release's atomic activation.
    const auto old = pimpl_->active.find(deployment_id);
    if (old != pimpl_->active.end() && old->second.request_id != request.request_id) {
        const auto old_session = pimpl_->sessions.find(old->second.request_id);
        if (old_session != pimpl_->sessions.end() && !old_session->second.validating) {
            pimpl_->staged_bytes -= old_session->second.staged_bytes;
            pimpl_->sessions.erase(old_session);
        }
    }
    pimpl_->active[deployment_id] = next;
    const std::size_t released_bytes = session.staged_bytes;
    pimpl_->staged_bytes -= released_bytes;
    pimpl_->sessions.erase(it);
    *receipt = next;
    if (error) error->clear();
    return true;
}

bool HandoffReceiver::get_active_release(const std::string& deployment_id,
                                         HandoffReceipt* receipt) const {
    if (!receipt) return false;
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    const auto it = pimpl_->active.find(deployment_id);
    if (it == pimpl_->active.end()) return false;
    *receipt = it->second;
    return true;
}

} // namespace nvlink::broker::wasm_handoff
