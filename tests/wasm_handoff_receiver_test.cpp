// SPDX-License-Identifier: MIT
#include "wasm_handoff_receiver.h"

#include <openssl/evp.h>

#include <array>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <utility>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << "CHECK failed: " #condition << " at " << __FILE__ << ':' << __LINE__ << "\n"; \
        std::exit(EXIT_FAILURE); \
    } \
} while (false)

int main() {
    using namespace nvlink::broker::wasm_handoff;
    const std::string message = "{\"contract\":\"wasm-handoff/1\",\"audience\":\"overhauled\"}";
    EVP_PKEY_CTX* raw_ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    CHECK(raw_ctx);
    CHECK(EVP_PKEY_keygen_init(raw_ctx) == 1);
    EVP_PKEY* raw_key = nullptr;
    CHECK(EVP_PKEY_keygen(raw_ctx, &raw_key) == 1);
    EVP_PKEY_CTX_free(raw_ctx);
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw_key, EVP_PKEY_free);

    TrustedKey trusted;
    std::size_t public_size = trusted.public_key.size();
    CHECK(EVP_PKEY_get_raw_public_key(key.get(), trusted.public_key.data(), &public_size) == 1);
    CHECK(public_size == trusted.public_key.size());
    trusted.issuer = "test-issuer";
    trusted.subject = "yukki-os";
    trusted.audience = "overhauled";
    TrustStore store;
    CHECK(store.add("test-key", trusted));

    Ed25519Signature signature{};
    std::size_t signature_size = signature.size();
    EVP_MD_CTX* sign_ctx = EVP_MD_CTX_new();
    CHECK(sign_ctx);
    CHECK(EVP_DigestSignInit(sign_ctx, nullptr, nullptr, nullptr, key.get()) == 1);
    CHECK(EVP_DigestSign(sign_ctx, signature.data(), &signature_size,
                          reinterpret_cast<const std::uint8_t*>(message.data()), message.size()) == 1);
    EVP_MD_CTX_free(sign_ctx);
    CHECK(signature_size == signature.size());
    CHECK(store.verify("test-key", "test-issuer", "yukki-os", "overhauled",
                        reinterpret_cast<const std::uint8_t*>(message.data()), message.size(), signature));
    CHECK(!store.verify("test-key", "test-issuer", "attacker", "overhauled",
                         reinterpret_cast<const std::uint8_t*>(message.data()), message.size(), signature));
    signature[0] ^= 1;
    CHECK(!store.verify("test-key", "test-issuer", "yukki-os", "overhauled",
                         reinterpret_cast<const std::uint8_t*>(message.data()), message.size(), signature));

    const auto now = std::chrono::system_clock::now();
    ReplayGuard replay(2);
    const std::string nonce(32, 'a');
    CHECK(replay.consume("yukki-os", "overhauled", nonce, now + std::chrono::minutes(1), now));
    CHECK(!replay.consume("yukki-os", "overhauled", nonce, now + std::chrono::minutes(1), now));
    CHECK(!replay.consume("yukki-os", "wrong-audience", nonce, now, now));

    const std::vector<std::uint8_t> artifact{'w','a','s','m'};
    const auto digest = sha256_hex(artifact.data(), artifact.size());
    ArtifactChunkReceiver receiver(artifact.size(), 64, digest);
    CHECK(!receiver.append(1, artifact.data(), 2, sha256_hex(artifact.data(), 2), false, nullptr));
    CHECK(receiver.append(0, artifact.data(), 2, sha256_hex(artifact.data(), 2), false, nullptr));
    CHECK(!receiver.append(1, artifact.data() + 1, 2, "sha256:bad", true, nullptr));
    CHECK(receiver.append(2, artifact.data() + 2, 2, sha256_hex(artifact.data() + 2, 2), true, nullptr));
    CHECK(receiver.complete());
    CHECK(receiver.bytes() == artifact);
    CHECK(!receiver.append(4, artifact.data(), 1, sha256_hex(artifact.data(), 1), true, nullptr));

    // Exercise the complete receiver transaction: sender authorization,
    // separate publisher provenance, bounded artifact transfer, module
    // validation, atomic runtime promotion, and signed receipt verification.
    auto create_signer = [](const std::string& issuer, const std::string& subject,
                            const std::string& key_id) {
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
        CHECK(ctx != nullptr);
        CHECK(EVP_PKEY_keygen_init(ctx) == 1);
        EVP_PKEY* raw = nullptr;
        CHECK(EVP_PKEY_keygen(ctx, &raw) == 1);
        EVP_PKEY_CTX_free(ctx);

        Ed25519PrivateKey private_key{};
        Ed25519PublicKey public_key{};
        std::size_t private_size = private_key.size();
        std::size_t public_size = public_key.size();
        CHECK(EVP_PKEY_get_raw_private_key(raw, private_key.data(), &private_size) == 1);
        CHECK(EVP_PKEY_get_raw_public_key(raw, public_key.data(), &public_size) == 1);
        EVP_PKEY_free(raw);
        CHECK(private_size == private_key.size());
        CHECK(public_size == public_key.size());
        return std::make_pair(
            Ed25519Signer(key_id, issuer, subject, private_key), public_key);
    };

    auto deployment_pair = create_signer("yukki-issuer", "node-7", "deployment-key");
    auto publisher_pair = create_signer("publisher-issuer", "release-bot", "publisher-key");
    auto broker_pair = create_signer("overhauled-issuer", "broker-1", "broker-receipt-key");
    auto& deployment_signer = deployment_pair.first;
    auto& publisher_signer = publisher_pair.first;
    auto& receipt_signer = broker_pair.first;

    TrustStore sender_trust;
    TrustedKey sender_key;
    sender_key.public_key = deployment_pair.second;
    sender_key.issuer = "yukki-issuer";
    sender_key.subject = "node-7";
    sender_key.audience = "overhauled";
    CHECK(sender_trust.add("deployment-key", sender_key));

    TrustStore publisher_trust;
    TrustedKey publisher_key;
    publisher_key.public_key = publisher_pair.second;
    publisher_key.issuer = "publisher-issuer";
    publisher_key.subject = "release-bot";
    publisher_key.audience = "wasm-artifact-publisher";
    CHECK(publisher_trust.add("publisher-key", publisher_key));

    TrustStore receipt_trust;
    TrustedKey receiver_key;
    receiver_key.public_key = broker_pair.second;
    receiver_key.issuer = "overhauled-issuer";
    receiver_key.subject = "broker-1";
    receiver_key.audience = "yukki-os";
    CHECK(receipt_trust.add("broker-receipt-key", receiver_key));

    HandoffPolicy policy;
    policy.audience = "overhauled";
    policy.receipt_audience = "yukki-os";
    policy.wasmtime_available = true;
    policy.allow_trusted_shared_store_workloads = true;
    policy.engine = "wasmtime";
    policy.engine_version = "51.0.0";
    policy.backend_id = "wasmtime-test";
    policy.available_capabilities = {"wasm-core-1"};
    policy.module_validator = [](const ModuleDescriptor&,
                                 const std::vector<std::uint8_t>& bytes,
                                 std::string* validation_error) {
        if (bytes.size() < 8 ||
            bytes[0] != 0x00 || bytes[1] != 0x61 || bytes[2] != 0x73 ||
            bytes[3] != 0x6d || bytes[4] != 0x01 || bytes[5] != 0x00 ||
            bytes[6] != 0x00 || bytes[7] != 0x00 || bytes.back() == 0xff) {
            if (validation_error) *validation_error = "test Wasm validator rejected module";
            return false;
        }
        return true;
    };
    bool runtime_promoted = false;
    policy.activate_release = [&runtime_promoted](
        const std::string& deployment, const std::string& release,
        const std::vector<VerifiedModuleView>& modules, std::string* activation_error) {
        if (deployment.empty() || release.empty() || modules.empty()) {
            if (activation_error) *activation_error = "empty release";
            return false;
        }
        runtime_promoted = true;
        return true;
    };

    ReplayGuard handoff_replay(64);
    HandoffReceiver handoff(sender_trust, publisher_trust, handoff_replay,
                            policy, receipt_signer);

    const auto now_tp = std::chrono::system_clock::now();
    const auto now_seconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            now_tp.time_since_epoch()).count());

    auto envelope_for = [&](const Ed25519Signer& signer, const std::string& audience,
                            char nonce_char, const std::string& message_id) {
        SignedMetadata envelope;
        envelope.message_id = message_id;
        envelope.issued_at = now_seconds;
        envelope.expires_at = now_seconds + 60;
        envelope.audience = audience;
        envelope.issuer = signer.issuer();
        envelope.subject = signer.subject();
        envelope.key_id = signer.key_id();
        envelope.nonce = std::string(32, nonce_char);
        return envelope;
    };

    auto make_prepare = [&](const std::vector<std::uint8_t>& wasm_bytes,
                            const std::string& release,
                            char nonce_char, const std::string& message_id) {
        PrepareRequest request;
        request.envelope = envelope_for(
            deployment_signer, "overhauled", nonce_char, message_id);
        request.request_id = message_id;
        request.deployment_id = "deployment-1";
        request.release_id = release;
        request.runtime_policy = "auto";
        request.requested_memory_bytes = 65536;
        request.requested_fuel = 100000;
        request.requested_wall_time_ms = 1000;
        request.requested_max_instances = 1;
        ModuleDescriptor module;
        module.module_id = "sample-module";
        module.version = "1.0.0";
        module.size_bytes = wasm_bytes.size();
        module.format = "wasm-core-1";
        module.digest = sha256_hex(wasm_bytes.data(), wasm_bytes.size());
        request.modules = {module};
        const std::string manifest = canonical_manifest(request.modules);
        request.manifest_digest = sha256_hex(
            reinterpret_cast<const std::uint8_t*>(manifest.data()), manifest.size());
        Digest raw_digest{};
        CHECK(parse_sha256(module.digest, &raw_digest));
        request.artifact_set_digest = sha256_hex(raw_digest.data(), raw_digest.size());

        request.publisher.issuer = publisher_signer.issuer();
        request.publisher.subject = publisher_signer.subject();
        request.publisher.key_id = publisher_signer.key_id();
        request.publisher.audience = "wasm-artifact-publisher";
        request.publisher.issued_at = now_seconds;
        request.publisher.expires_at = now_seconds + 60;
        const std::string publisher_canonical = canonical_publisher_attestation(request);
        request.publisher.signature = publisher_signer.sign(
            reinterpret_cast<const std::uint8_t*>(publisher_canonical.data()),
            publisher_canonical.size());
        const std::string canonical = canonical_prepare_message(request);
        request.envelope.signature = deployment_signer.sign(
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size());
        return request;
    };

    const std::vector<std::uint8_t> wasm_v1 = {
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00
    };
    PrepareRequest prepare = make_prepare(
        wasm_v1, "release-1", 'b',
        "11111111-1111-4111-8111-111111111111");
    const PrepareDecision decision = handoff.prepare(prepare, now_tp);
    CHECK(decision.accepted);
    CHECK(decision.decision == "accepted");
    CHECK(!decision.transfer_id.empty());

    ArtifactChunkMessage chunk;
    chunk.envelope = envelope_for(
        deployment_signer, "overhauled", 'c',
        "22222222-2222-4222-8222-222222222222");
    chunk.transfer_id = decision.transfer_id;
    chunk.artifact_digest = prepare.modules[0].digest;
    chunk.offset = 0;
    chunk.total_length = wasm_v1.size();
    chunk.data = wasm_v1;
    chunk.chunk_digest = sha256_hex(chunk.data.data(), chunk.data.size());
    chunk.final = true;
    const std::string chunk_canonical = canonical_chunk_message(chunk);
    chunk.envelope.signature = deployment_signer.sign(
        reinterpret_cast<const std::uint8_t*>(chunk_canonical.data()),
        chunk_canonical.size());
    std::string handoff_error;
    CHECK(handoff.append_chunk(chunk, &handoff_error, now_tp));

    ActivationRequest activation;
    activation.envelope = envelope_for(
        deployment_signer, "overhauled", 'd',
        "33333333-3333-4333-8333-333333333333");
    activation.request_id = prepare.request_id;
    activation.transfer_id = decision.transfer_id;
    const std::string activation_canonical = canonical_activation_message(activation);
    activation.envelope.signature = deployment_signer.sign(
        reinterpret_cast<const std::uint8_t*>(activation_canonical.data()),
        activation_canonical.size());
    HandoffReceipt receipt;
    CHECK(handoff.activate(activation, &receipt, &handoff_error, now_tp));
    CHECK(runtime_promoted);
    CHECK(receipt.state == "activated");
    CHECK(receipt.backend_id == "wasmtime-test");
    const std::string receipt_canonical = canonical_receipt(receipt);
    CHECK(receipt_trust.verify(
        receipt.signer_key_id, receipt.issuer, receipt.subject, receipt.audience,
        reinterpret_cast<const std::uint8_t*>(receipt_canonical.data()),
        receipt_canonical.size(), receipt.signature));
    HandoffReceipt active_receipt;
    CHECK(handoff.get_active_release("deployment-1", &active_receipt));
    CHECK(active_receipt.release_id == "release-1");

    // A subsequent artifact that fails module validation cannot replace the
    // active release and its staged bytes are discarded.
    auto wasm_bad = wasm_v1;
    wasm_bad.push_back(0xff);
    PrepareRequest bad_prepare = make_prepare(
        wasm_bad, "release-bad", 'e',
        "44444444-4444-4444-8444-444444444444");
    const PrepareDecision bad_decision = handoff.prepare(bad_prepare, now_tp);
    CHECK(bad_decision.accepted);

    ArtifactChunkMessage bad_chunk;
    bad_chunk.envelope = envelope_for(
        deployment_signer, "overhauled", 'f',
        "55555555-5555-4555-8555-555555555555");
    bad_chunk.transfer_id = bad_decision.transfer_id;
    bad_chunk.artifact_digest = bad_prepare.modules[0].digest;
    bad_chunk.offset = 0;
    bad_chunk.total_length = wasm_bad.size();
    bad_chunk.data = wasm_bad;
    bad_chunk.chunk_digest = sha256_hex(bad_chunk.data.data(), bad_chunk.data.size());
    bad_chunk.final = true;
    const std::string bad_chunk_canonical = canonical_chunk_message(bad_chunk);
    bad_chunk.envelope.signature = deployment_signer.sign(
        reinterpret_cast<const std::uint8_t*>(bad_chunk_canonical.data()),
        bad_chunk_canonical.size());
    CHECK(handoff.append_chunk(bad_chunk, &handoff_error, now_tp));

    ActivationRequest bad_activation;
    bad_activation.envelope = envelope_for(
        deployment_signer, "overhauled", 'g',
        "66666666-6666-4666-8666-666666666666");
    bad_activation.request_id = bad_prepare.request_id;
    bad_activation.transfer_id = bad_decision.transfer_id;
    const std::string bad_activation_canonical = canonical_activation_message(bad_activation);
    bad_activation.envelope.signature = deployment_signer.sign(
        reinterpret_cast<const std::uint8_t*>(bad_activation_canonical.data()),
        bad_activation_canonical.size());
    CHECK(!handoff.activate(bad_activation, &receipt, &handoff_error, now_tp));
    CHECK(handoff.get_active_release("deployment-1", &active_receipt));
    CHECK(active_receipt.release_id == "release-1");
    return 0;
}
