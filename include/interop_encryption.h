// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <vector>
#include <array>
#include <string>

namespace nvlink::interop {

enum class AeadId : uint8_t {
  ChaCha20Poly1305 = 1,
  Aes256Gcm = 2,
};

struct SessionConfig {
  uint16_t version = 1;
  AeadId aead = AeadId::ChaCha20Poly1305;
  std::array<uint8_t, 32> key{};
  uint64_t sequence = 0;
};

class Session {
public:
  Session();
  ~Session();

  // Initialize with classical shared secret and optional PQC secret
  // classical_secret: raw shared secret bytes (e.g., X25519 shared secret)
  // pqc_secret: optional PQC shared secret bytes (can be empty)
  bool initialize(const std::vector<uint8_t>& classical_secret,
                  const std::vector<uint8_t>& pqc_secret,
                  std::string* error = nullptr);

  // Encrypt plaintext with associated data (AAD). Returns ciphertext (tag appended) or empty on error.
  std::vector<uint8_t> encrypt(const std::vector<uint8_t>& plaintext,
                               const std::vector<uint8_t>& aad,
                               std::string* error = nullptr);

  // Decrypt ciphertext with associated data (AAD). Returns plaintext or empty on error.
  std::vector<uint8_t> decrypt(const std::vector<uint8_t>& ciphertext,
                               const std::vector<uint8_t>& aad,
                               std::string* error = nullptr);

  const SessionConfig& config() const { return cfg_; }

private:
  SessionConfig cfg_;
  // Implementation-specific opaque pointers
  void* impl_ctx_ = nullptr;
};

} // namespace nvlink::interop
