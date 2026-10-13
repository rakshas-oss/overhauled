// SPDX-License-Identifier: MIT
#include "interop_encryption.h"

#ifdef OVERHAULED_INTEROP_OPENSSL
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/err.h>
#endif

#include <array>
#include <cstring>

namespace nvlink::interop {
namespace {

#ifdef OVERHAULED_INTEROP_OPENSSL
bool hkdf_sha256(const std::vector<uint8_t>& ikm, uint8_t* out, size_t out_len) {
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
    if (!pctx) {
        return false;
    }
    if (EVP_PKEY_derive_init(pctx) <= 0) {
        EVP_PKEY_CTX_free(pctx);
        return false;
    }
    if (EVP_PKEY_CTX_set_hkdf_md(pctx, EVP_sha256()) <= 0) {
        EVP_PKEY_CTX_free(pctx);
        return false;
    }
    if (EVP_PKEY_CTX_set1_hkdf_salt(pctx, nullptr, 0) <= 0) {
        EVP_PKEY_CTX_free(pctx);
        return false;
    }
    if (EVP_PKEY_CTX_set1_hkdf_key(pctx, ikm.data(), static_cast<int>(ikm.size())) <= 0) {
        EVP_PKEY_CTX_free(pctx);
        return false;
    }
    static const unsigned char info[] = "overhauled-interop-v1";
    if (EVP_PKEY_CTX_add1_hkdf_info(pctx, info, sizeof(info) - 1) <= 0) {
        EVP_PKEY_CTX_free(pctx);
        return false;
    }
    size_t len = out_len;
    int rc = EVP_PKEY_derive(pctx, out, &len);
    EVP_PKEY_CTX_free(pctx);
    return rc == 1 && len == out_len;
}
#endif

}  // namespace

Session::Session() : impl_ctx_(nullptr) {}
Session::~Session() {
    impl_ctx_ = nullptr;
}

bool Session::initialize(const std::vector<uint8_t>& classical_secret,
                        const std::vector<uint8_t>& pqc_secret,
                        std::string* error) {
    std::vector<uint8_t> ikm;
    ikm.reserve(classical_secret.size() + pqc_secret.size());
    ikm.insert(ikm.end(), classical_secret.begin(), classical_secret.end());
    if (!pqc_secret.empty()) {
        ikm.insert(ikm.end(), pqc_secret.begin(), pqc_secret.end());
    }
    if (ikm.empty()) {
        if (error != nullptr) {
            *error = "empty shared secret";
        }
        return false;
    }

#ifdef OVERHAULED_INTEROP_OPENSSL
    std::array<uint8_t, 32> key{};
    if (!hkdf_sha256(ikm, key.data(), key.size())) {
        if (error != nullptr) {
            *error = "interop key derivation failed";
        }
        return false;
    }
    std::memcpy(cfg_.key.data(), key.data(), key.size());
    OPENSSL_cleanse(key.data(), key.size());
    cfg_.aead = AeadId::Aes256Gcm;
    cfg_.sequence = 0;
    return true;
#else
    std::array<uint8_t, 32> key{};
    for (size_t i = 0; i < ikm.size(); ++i) {
        key[i % key.size()] ^= ikm[i];
    }
    std::memcpy(cfg_.key.data(), key.data(), key.size());
    cfg_.aead = AeadId::ChaCha20Poly1305;
    cfg_.sequence = 0;
    return true;
#endif
}

std::vector<uint8_t> Session::encrypt(const std::vector<uint8_t>& payload,
                                      const std::vector<uint8_t>& aad,
                                      std::string* error) {
#ifdef OVERHAULED_INTEROP_OPENSSL
    const EVP_CIPHER* cipher = (cfg_.aead == AeadId::Aes256Gcm) ? EVP_aes_256_gcm() : EVP_chacha20_poly1305();
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        if (error != nullptr) {
            *error = "cipher context allocation failed";
        }
        return {};
    }

    uint8_t nonce[12] = {0};
    const uint64_t seq = cfg_.sequence++;
    for (int i = 0; i < 8; ++i) {
        nonce[i] = static_cast<uint8_t>((seq >> (56 - (i * 8))) & 0xff);
    }

    if (EVP_EncryptInit_ex(ctx, cipher, nullptr, nullptr, nullptr) <= 0 ||
        EVP_EncryptInit_ex(ctx, nullptr, nullptr, cfg_.key.data(), nonce) <= 0) {
        EVP_CIPHER_CTX_free(ctx);
        if (error != nullptr) {
            *error = "cipher init failed";
        }
        return {};
    }

    int out_len = 0;
    std::vector<uint8_t> output(payload.size() + 16 + 16, 0);
    if (!aad.empty()) {
        int aad_len = 0;
        if (EVP_EncryptUpdate(ctx, nullptr, &aad_len, aad.data(), static_cast<int>(aad.size())) <= 0) {
            EVP_CIPHER_CTX_free(ctx);
            if (error != nullptr) {
                *error = "AAD update failed";
            }
            return {};
        }
    }

    int cipher_len = 0;
    if (EVP_EncryptUpdate(ctx, output.data(), &cipher_len, payload.data(), static_cast<int>(payload.size())) <= 0) {
        EVP_CIPHER_CTX_free(ctx);
        if (error != nullptr) {
            *error = "encrypt update failed";
        }
        return {};
    }

    int final_len = 0;
    if (EVP_EncryptFinal_ex(ctx, output.data() + cipher_len, &final_len) <= 0) {
        EVP_CIPHER_CTX_free(ctx);
        if (error != nullptr) {
            *error = "encrypt final failed";
        }
        return {};
    }

    unsigned char tag[16] = {0};
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) <= 0) {
        EVP_CIPHER_CTX_free(ctx);
        if (error != nullptr) {
            *error = "get tag failed";
        }
        return {};
    }

    EVP_CIPHER_CTX_free(ctx);

    std::vector<uint8_t> result;
    result.insert(result.end(), output.begin(), output.begin() + cipher_len + final_len);
    result.insert(result.end(), tag, tag + 16);
    return result;
#else
    if (error != nullptr) {
        *error = "interop encryption requires OpenSSL in this build";
    }
    return {};
#endif
}

std::vector<uint8_t> Session::decrypt(const std::vector<uint8_t>& ciphertext,
                                      const std::vector<uint8_t>& aad,
                                      std::string* error) {
#ifdef OVERHAULED_INTEROP_OPENSSL
    if (ciphertext.size() < 16) {
        if (error != nullptr) {
            *error = "ciphertext too short";
        }
        return {};
    }

    const EVP_CIPHER* cipher = (cfg_.aead == AeadId::Aes256Gcm) ? EVP_aes_256_gcm() : EVP_chacha20_poly1305();
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        if (error != nullptr) {
            *error = "cipher context allocation failed";
        }
        return {};
    }

    uint8_t nonce[12] = {0};
    const uint64_t seq = cfg_.sequence++;
    for (int i = 0; i < 8; ++i) {
        nonce[i] = static_cast<uint8_t>((seq >> (56 - (i * 8))) & 0xff);
    }

    if (EVP_DecryptInit_ex(ctx, cipher, nullptr, nullptr, nullptr) <= 0 ||
        EVP_DecryptInit_ex(ctx, nullptr, nullptr, cfg_.key.data(), nonce) <= 0) {
        EVP_CIPHER_CTX_free(ctx);
        if (error != nullptr) {
            *error = "decrypt init failed";
        }
        return {};
    }

    if (!aad.empty()) {
        int aad_len = 0;
        if (EVP_DecryptUpdate(ctx, nullptr, &aad_len, aad.data(), static_cast<int>(aad.size())) <= 0) {
            EVP_CIPHER_CTX_free(ctx);
            if (error != nullptr) {
                *error = "AAD update failed";
            }
            return {};
        }
    }

    const size_t message_len = ciphertext.size() - 16;
    std::vector<uint8_t> output(message_len, 0);
    int out_len = 0;
    if (EVP_DecryptUpdate(ctx, output.data(), &out_len, ciphertext.data(), static_cast<int>(message_len)) <= 0) {
        EVP_CIPHER_CTX_free(ctx);
        if (error != nullptr) {
            *error = "decrypt update failed";
        }
        return {};
    }

    if (cfg_.aead == AeadId::Aes256Gcm) {
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, const_cast<uint8_t*>(ciphertext.data() + message_len)) <= 0) {
            EVP_CIPHER_CTX_free(ctx);
            if (error != nullptr) {
                *error = "decrypt tag failed";
            }
            return {};
        }
    } else {
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 16, const_cast<uint8_t*>(ciphertext.data() + message_len)) <= 0) {
            EVP_CIPHER_CTX_free(ctx);
            if (error != nullptr) {
                *error = "decrypt tag failed";
            }
            return {};
        }
    }

    int final_len = 0;
    if (EVP_DecryptFinal_ex(ctx, output.data() + out_len, &final_len) <= 0) {
        EVP_CIPHER_CTX_free(ctx);
        if (error != nullptr) {
            *error = "decrypt final failed";
        }
        return {};
    }

    EVP_CIPHER_CTX_free(ctx);
    output.resize(static_cast<size_t>(out_len + final_len));
    return output;
#else
    if (error != nullptr) {
        *error = "interop encryption requires OpenSSL in this build";
    }
    return {};
#endif
}

}  // namespace nvlink::interop
