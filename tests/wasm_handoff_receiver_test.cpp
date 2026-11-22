// SPDX-License-Identifier: MIT
#include "wasm_handoff_receiver.h"

#include <openssl/evp.h>

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

int main() {
    using namespace nvlink::broker::wasm_handoff;
    const std::string message = "{\"contract\":\"wasm-handoff/1\",\"audience\":\"overhauled\"}";
    EVP_PKEY_CTX* raw_ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    assert(raw_ctx);
    assert(EVP_PKEY_keygen_init(raw_ctx) == 1);
    EVP_PKEY* raw_key = nullptr;
    assert(EVP_PKEY_keygen(raw_ctx, &raw_key) == 1);
    EVP_PKEY_CTX_free(raw_ctx);
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw_key, EVP_PKEY_free);

    TrustedKey trusted;
    std::size_t public_size = trusted.public_key.size();
    assert(EVP_PKEY_get_raw_public_key(key.get(), trusted.public_key.data(), &public_size) == 1);
    assert(public_size == trusted.public_key.size());
    trusted.issuer = "test-issuer";
    trusted.subject = "yukki-os";
    trusted.audience = "overhauled";
    TrustStore store;
    assert(store.add("test-key", trusted));

    Ed25519Signature signature{};
    std::size_t signature_size = signature.size();
    EVP_MD_CTX* sign_ctx = EVP_MD_CTX_new();
    assert(sign_ctx);
    assert(EVP_DigestSignInit(sign_ctx, nullptr, nullptr, nullptr, key.get()) == 1);
    assert(EVP_DigestSign(sign_ctx, signature.data(), &signature_size,
                          reinterpret_cast<const std::uint8_t*>(message.data()), message.size()) == 1);
    EVP_MD_CTX_free(sign_ctx);
    assert(signature_size == signature.size());
    assert(store.verify("test-key", "test-issuer", "yukki-os", "overhauled",
                        reinterpret_cast<const std::uint8_t*>(message.data()), message.size(), signature));
    assert(!store.verify("test-key", "test-issuer", "attacker", "overhauled",
                         reinterpret_cast<const std::uint8_t*>(message.data()), message.size(), signature));
    signature[0] ^= 1;
    assert(!store.verify("test-key", "test-issuer", "yukki-os", "overhauled",
                         reinterpret_cast<const std::uint8_t*>(message.data()), message.size(), signature));

    const auto now = std::chrono::system_clock::now();
    ReplayGuard replay(2);
    const std::string nonce(32, 'a');
    assert(replay.consume("yukki-os", "overhauled", nonce, now + std::chrono::minutes(1), now));
    assert(!replay.consume("yukki-os", "overhauled", nonce, now + std::chrono::minutes(1), now));
    assert(!replay.consume("yukki-os", "wrong-audience", nonce, now, now));

    const std::vector<std::uint8_t> artifact{'w','a','s','m'};
    const auto digest = sha256_hex(artifact.data(), artifact.size());
    ArtifactChunkReceiver receiver(artifact.size(), 64, digest);
    assert(!receiver.append(1, artifact.data(), 2, sha256_hex(artifact.data(), 2), false, nullptr));
    assert(receiver.append(0, artifact.data(), 2, sha256_hex(artifact.data(), 2), false, nullptr));
    assert(!receiver.append(1, artifact.data() + 1, 2, "sha256:bad", true, nullptr));
    assert(receiver.append(2, artifact.data() + 2, 2, sha256_hex(artifact.data() + 2, 2), true, nullptr));
    assert(receiver.complete());
    assert(receiver.bytes() == artifact);
    assert(!receiver.append(4, artifact.data(), 1, sha256_hex(artifact.data(), 1), true, nullptr));
    return 0;
}
