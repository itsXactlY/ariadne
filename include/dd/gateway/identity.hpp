#pragma once
// dd/gateway/identity.hpp — the gateway's TLS identity: one self-signed certificate
// and its key, and the fingerprint a phone pins.
//
// No CA. The pairing code carries fp = base64(sha256(SubjectPublicKeyInfo)), the
// phone fetches the certificate once, checks it against fp and pins it; the relay
// knows the same identity as hex(sha256(SPKI)) and makes the gateway sign a nonce
// with the matching key before it routes anything to it.
//
// A fresh home gets an EC P-256 pair; an operator moving off hermes-crypto adopts
// that gateway's cert and key instead, so phones paired against it keep their pin.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace dd {
namespace gateway {

struct Identity {
    std::string cert_pem, key_pem;
    std::string spki_sha256;   // 32 raw bytes
    std::string fp_b64() const;   // what the pairing code and the app's pin carry
    std::string fp_hex() const;   // what the relay routes on
    std::string cert_der() const;
};

// <dir>/cert.pem + <dir>/key.pem, made on first use (key 0600). nullopt + error when a
// pair exists but does not load, or the files cannot be written.
std::optional<Identity> load_or_create(const std::filesystem::path& dir, std::string& error);
std::optional<Identity> load(const std::filesystem::path& cert, const std::filesystem::path& key, std::string& error);
// Copy another gateway's pair into <dir> (refuses to overwrite one that is there).
bool adopt(const std::filesystem::path& cert, const std::filesystem::path& key, const std::filesystem::path& dir,
           std::string& error);

// An SSL_CTX* (TLS 1.2+) serving this identity, for net::Server::set_tls. The caller
// owns it for the server's life; free_server_ctx releases it.
void* make_server_ctx(const Identity& id, std::string& error);
void free_server_ctx(void* ctx);

// sha256 signature of `message` with the identity's key: ECDSA (DER) for an EC key,
// PKCS#1 v1.5 for RSA -- what relay.py verifies. "" on failure.
std::string sign_sha256(const Identity& id, std::string_view message);

}  // namespace gateway
}  // namespace dd
