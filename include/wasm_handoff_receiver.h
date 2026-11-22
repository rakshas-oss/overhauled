// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace nvlink::broker::wasm_handoff {

using Digest = std::array<std::uint8_t, 32>;
using Ed25519PublicKey = std::array<std::uint8_t, 32>;
using Ed25519PrivateKey = std::array<std::uint8_t, 32>;
using Ed25519Signature = std::array<std::uint8_t, 64>;

std::string sha256_hex(const std::uint8_t* data, std::size_t size);
bool parse_sha256(const std::string& value, Digest* out);

// A message is signed over the contract-defined canonical JSON envelope with
// the signature member omitted. Canonical bytes are reconstructed from the
// typed request by the receiver; untrusted callers cannot supply separate
// verification bytes and metadata that disagree.
struct SignedMetadata {
    std::string message_id;
    std::uint64_t issued_at = 0;
    std::uint64_t expires_at = 0;
    std::string audience;
    std::string issuer;
    std::string subject;
    std::string key_id;
    std::string nonce;
    Ed25519Signature signature{};
};

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

class Ed25519Signer {
public:
    Ed25519Signer(const std::string& key_id, const std::string& issuer,
                  const std::string& subject, const Ed25519PrivateKey& private_key);
    Ed25519Signer(const Ed25519Signer&) = delete;
    Ed25519Signer& operator=(const Ed25519Signer&) = delete;
    Ed25519Signer(Ed25519Signer&&) noexcept;
    Ed25519Signer& operator=(Ed25519Signer&&) noexcept;
    ~Ed25519Signer();

    const std::string& key_id() const noexcept;
    const std::string& issuer() const noexcept;
    const std::string& subject() const noexcept;
    Ed25519Signature sign(const std::uint8_t* message, std::size_t size) const;
    Ed25519PublicKey public_key() const;

private:
    std::string key_id_;
    std::string issuer_;
    std::string subject_;
    void* key_ = nullptr; // owned EVP_PKEY; kept opaque in the public header
};

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
// chunks. It deliberately rejects retransmission: restart with a new transfer.
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

struct ModuleDescriptor {
    std::string module_id;
    std::string version;
    std::string digest;
    std::uint64_t size_bytes = 0;
    std::string format = "wasm-core-1";
};

struct PublisherAttestation {
    std::string issuer;
    std::string subject;
    std::string key_id;
    std::string audience = "wasm-artifact-publisher";
    std::uint64_t issued_at = 0;
    std::uint64_t expires_at = 0;
    Ed25519Signature signature{};
};

struct PrepareRequest {
    SignedMetadata envelope;
    std::string request_id;
    std::string deployment_id;
    std::string release_id;
    std::string artifact_set_digest;
    std::string manifest_digest;
    std::vector<ModuleDescriptor> modules;
    std::string runtime_policy = "auto";
    std::string preferred_backend;
    std::vector<std::string> required_capabilities;
    std::uint64_t requested_memory_bytes = 0;
    std::uint64_t requested_fuel = 0;
    std::uint64_t requested_wall_time_ms = 0;
    std::uint32_t requested_max_instances = 1;
    PublisherAttestation publisher;
};

struct ArtifactChunkMessage {
    SignedMetadata envelope;
    std::string transfer_id;
    std::string artifact_digest;
    std::uint64_t offset = 0;
    std::uint64_t total_length = 0;
    std::string chunk_digest;
    std::vector<std::uint8_t> data;
    bool final = false;
};

struct ActivationRequest {
    SignedMetadata envelope;
    std::string request_id;
    std::string transfer_id;
};

struct VerifiedModuleView {
    const ModuleDescriptor* descriptor = nullptr;
    const std::vector<std::uint8_t>* bytes = nullptr;
};

struct HandoffPolicy {
    std::string audience = "overhauled";
    std::string receipt_audience = "yukkios";
    std::size_t maximum_modules = 256;
    std::size_t maximum_module_bytes = 64U * 1024U * 1024U;
    std::size_t maximum_total_bytes = 256U * 1024U * 1024U;
    std::size_t maximum_chunk_bytes = 4U * 1024U * 1024U;
    std::size_t chunk_size_bytes = 1024U * 1024U;
    std::uint64_t maximum_message_lifetime_seconds = 300;
    std::uint64_t clock_skew_seconds = 30;
    std::uint64_t maximum_memory_bytes = 256U * 1024U * 1024U;
    std::uint64_t maximum_fuel = 1000000000ULL;
    std::uint64_t maximum_wall_time_ms = 60000;
    std::uint32_t maximum_instances = 32;
    std::vector<std::string> available_capabilities;
    std::string backend_id = "wasmtime-default";
    std::string engine = "wasmtime";
    std::string engine_version;
    std::string abi_profile = "wasm-core-1";
    bool wasmtime_available = false;
    // Must perform real Wasmtime compile/validation without activating the module.
    // The receiver fails closed when no validator is installed.
    std::function<bool(const ModuleDescriptor&, const std::vector<std::uint8_t>&, std::string*)> module_validator;
    // Must stage the complete validated release and atomically make it active.
    // Return false without changing the previous active release on any failure.
    std::function<bool(const std::string&, const std::string&,
                       const std::vector<VerifiedModuleView>&, std::string*)> activate_release;
    // The current shared-Store Wasmtime embedding is only suitable for
    // operator-authorized trusted artifacts. This is false by default.
    bool allow_trusted_shared_store_workloads = false;
    std::size_t maximum_concurrent_sessions = 16;
    std::size_t maximum_staged_bytes = 512U * 1024U * 1024U;
    std::size_t maximum_active_deployments = 256;
    double message_rate_per_second = 100.0;
    std::size_t message_burst = 200;
    double per_sender_message_rate_per_second = 20.0;
    std::size_t per_sender_message_burst = 40;
    std::size_t maximum_rate_limit_subjects = 1024;
    std::size_t ingress_byte_burst = 16U * 1024U * 1024U;
    std::size_t per_sender_ingress_byte_burst = 8U * 1024U * 1024U;
    double ingress_bytes_per_second = 32.0 * 1024.0 * 1024.0;
    double per_sender_ingress_bytes_per_second = 8.0 * 1024.0 * 1024.0;
};

struct PrepareDecision {
    bool accepted = false;
    std::string request_id;
    std::string decision = "rejected";
    std::string rejection_code;
    std::string transfer_id;
    std::size_t chunk_size_bytes = 0;
    std::string backend_id;
    std::string engine;
    std::string engine_version;
    std::string abi_profile;
    std::vector<std::string> accepted_capabilities;
};

struct HandoffReceipt {
    std::string request_id;
    std::string deployment_id;
    std::string release_id;
    std::string state;
    std::string backend_id;
    std::string engine;
    std::string engine_version;
    std::string abi_profile;
    std::string artifact_set_digest;
    std::uint64_t issued_at = 0;
    std::uint64_t expires_at = 0;
    std::string message_id;
    std::string nonce;
    std::string audience;
    std::string issuer;
    std::string subject;
    std::string signer_key_id;
    Ed25519Signature signature{};
};

std::string canonical_manifest(const std::vector<ModuleDescriptor>& modules);
std::string canonical_prepare_message(const PrepareRequest& request);
std::string canonical_chunk_message(const ArtifactChunkMessage& message);
std::string canonical_activation_message(const ActivationRequest& request);
std::string canonical_publisher_attestation(const PrepareRequest& request);
std::string canonical_receipt(const HandoffReceipt& receipt);

class HandoffReceiver {
public:
    HandoffReceiver(const TrustStore& sender_trust,
                    const TrustStore& publisher_trust,
                    ReplayGuard& replay_guard,
                    const HandoffPolicy& policy,
                    const Ed25519Signer& receipt_signer);
    ~HandoffReceiver();

    PrepareDecision prepare(const PrepareRequest& request,
                            std::chrono::system_clock::time_point now =
                                std::chrono::system_clock::now());
    bool append_chunk(const ArtifactChunkMessage& chunk, std::string* error,
                      std::chrono::system_clock::time_point now =
                          std::chrono::system_clock::now());
    bool activate(const ActivationRequest& request, HandoffReceipt* receipt,
                  std::string* error,
                  std::chrono::system_clock::time_point now =
                      std::chrono::system_clock::now());
    bool get_active_release(const std::string& deployment_id,
                            HandoffReceipt* receipt) const;

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace nvlink::broker::wasm_handoff
