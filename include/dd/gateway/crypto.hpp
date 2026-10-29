#pragma once
// dd/gateway/crypto.hpp — the primitives the device gateway stands on, over OpenSSL.
//
// The envelope is the one hermes-crypto's CryptoMiddleware and the Mazemaker app's
// GatewayCrypto already speak: base64(nonce16 | tag16 | ciphertext), AES-256-GCM with
// a 16-byte nonce and no associated data. Anything that changes it breaks every paired
// phone, so it is pinned by a known-answer test (tests/device_gateway_test.cpp).
//
// Byte strings travel as std::string. Nothing here keeps state.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace dd {
namespace gateway {
namespace crypto {

std::string b64(std::string_view bytes);                     // standard alphabet, padded
std::optional<std::string> unb64(std::string_view text);     // standard alphabet; whitespace ignored
std::string hex(std::string_view bytes);                     // lowercase
std::optional<std::string> unhex(std::string_view text);

// n bytes from the OS CSPRNG (RAND_bytes). Throws std::runtime_error when it has none:
// a key from a failed RNG is worse than no key.
std::string random_bytes(size_t n);

std::string sha256(std::string_view data);                                  // 32 raw bytes
std::string hmac_sha256(std::string_view key, std::string_view message);   // 32 raw bytes
std::string hkdf_sha256(std::string_view ikm, std::string_view salt, std::string_view info, size_t length);

// Constant time over the shorter length, false on a length mismatch.
bool equal(std::string_view a, std::string_view b);

// The gateway envelope. `key` is 32 raw bytes; seal() draws a fresh nonce every call.
std::string seal(std::string_view key, std::string_view plaintext);
std::optional<std::string> open(std::string_view key, std::string_view sealed_b64);
// seal() with a caller's nonce: for the known-answer test only.
std::string seal_with_nonce(std::string_view key, std::string_view nonce16, std::string_view plaintext);

}  // namespace crypto
}  // namespace gateway
}  // namespace dd
