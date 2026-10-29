// src/gateway/crypto.cpp — impl of dd/gateway/crypto.hpp.

#include "dd/gateway/crypto.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <memory>
#include <stdexcept>

namespace dd {
namespace gateway {
namespace crypto {

namespace {

constexpr size_t kNonce = 16, kTag = 16, kKey = 32;

const unsigned char* u(std::string_view s) { return reinterpret_cast<const unsigned char*>(s.data()); }

struct CtxFree {
    void operator()(EVP_CIPHER_CTX* c) const { EVP_CIPHER_CTX_free(c); }
};
using Cipher = std::unique_ptr<EVP_CIPHER_CTX, CtxFree>;

}  // namespace

std::string b64(std::string_view bytes) {
    std::string out(4 * ((bytes.size() + 2) / 3), '\0');
    const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), u(bytes), static_cast<int>(bytes.size()));
    out.resize(static_cast<size_t>(n));
    return out;
}

std::optional<std::string> unb64(std::string_view text) {
    std::string clean;
    clean.reserve(text.size());
    for (char c : text)
        if (c != ' ' && c != '\n' && c != '\r' && c != '\t') clean += c;
    if (clean.size() % 4 != 0) return std::nullopt;
    if (clean.empty()) return std::string();
    std::string out(clean.size() / 4 * 3, '\0');
    const int n = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()), u(clean), static_cast<int>(clean.size()));
    if (n < 0) return std::nullopt;
    // EVP_DecodeBlock counts padding as zero bytes; take them back off.
    size_t len = static_cast<size_t>(n);
    if (clean.back() == '=') --len;
    if (clean.size() > 1 && clean[clean.size() - 2] == '=') --len;
    out.resize(len);
    return out;
}

std::string hex(std::string_view bytes) {
    static const char* d = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) {
        out += d[c >> 4];
        out += d[c & 15];
    }
    return out;
}

std::optional<std::string> unhex(std::string_view text) {
    if (text.size() % 2) return std::nullopt;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2) {
        const int hi = nib(text[i]), lo = nib(text[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out += static_cast<char>(hi << 4 | lo);
    }
    return out;
}

std::string random_bytes(size_t n) {
    std::string out(n, '\0');
    if (n && RAND_bytes(reinterpret_cast<unsigned char*>(out.data()), static_cast<int>(n)) != 1)
        throw std::runtime_error("no randomness from the OS");
    return out;
}

std::string sha256(std::string_view data) {
    std::string out(32, '\0');
    unsigned int len = 0;
    EVP_Digest(data.data(), data.size(), reinterpret_cast<unsigned char*>(out.data()), &len, EVP_sha256(), nullptr);
    return out;
}

std::string hmac_sha256(std::string_view key, std::string_view message) {
    std::string out(32, '\0');
    unsigned int len = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), u(message), message.size(),
         reinterpret_cast<unsigned char*>(out.data()), &len);
    return out;
}

std::string hkdf_sha256(std::string_view ikm, std::string_view salt, std::string_view info, size_t length) {
    // RFC 5869 by hand on HMAC: extract, then expand. Small, and identical across OpenSSL 1.1 and 3.
    const std::string prk = hmac_sha256(salt.empty() ? std::string(32, '\0') : std::string(salt), ikm);
    std::string out, t;
    for (unsigned char i = 1; out.size() < length; ++i) {
        t = hmac_sha256(prk, t + std::string(info) + static_cast<char>(i));
        out += t;
    }
    out.resize(length);
    return out;
}

bool equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

std::string seal_with_nonce(std::string_view key, std::string_view nonce, std::string_view plaintext) {
    if (key.size() != kKey || nonce.size() != kNonce) throw std::invalid_argument("seal: key 32, nonce 16");
    Cipher c(EVP_CIPHER_CTX_new());
    std::string ct(plaintext.size(), '\0'), tag(kTag, '\0');
    int len = 0, fin = 0;
    if (!c || EVP_EncryptInit_ex(c.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(c.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kNonce), nullptr) != 1 ||
        EVP_EncryptInit_ex(c.get(), nullptr, nullptr, u(key), u(nonce)) != 1 ||
        EVP_EncryptUpdate(c.get(), reinterpret_cast<unsigned char*>(ct.data()), &len, u(plaintext),
                          static_cast<int>(plaintext.size())) != 1 ||
        EVP_EncryptFinal_ex(c.get(), reinterpret_cast<unsigned char*>(ct.data()) + len, &fin) != 1 ||
        EVP_CIPHER_CTX_ctrl(c.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(kTag), tag.data()) != 1)
        throw std::runtime_error("seal: AES-GCM failed");
    ct.resize(static_cast<size_t>(len + fin));
    return b64(std::string(nonce) + tag + ct);
}

std::string seal(std::string_view key, std::string_view plaintext) {
    return seal_with_nonce(key, random_bytes(kNonce), plaintext);
}

std::optional<std::string> open(std::string_view key, std::string_view sealed_b64) {
    if (key.size() != kKey) return std::nullopt;
    const auto raw = unb64(sealed_b64);
    if (!raw || raw->size() < kNonce + kTag) return std::nullopt;
    const std::string_view r(*raw);
    const std::string_view nonce = r.substr(0, kNonce), tag = r.substr(kNonce, kTag), ct = r.substr(kNonce + kTag);
    Cipher c(EVP_CIPHER_CTX_new());
    std::string pt(ct.size(), '\0');
    int len = 0, fin = 0;
    if (!c || EVP_DecryptInit_ex(c.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(c.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kNonce), nullptr) != 1 ||
        EVP_DecryptInit_ex(c.get(), nullptr, nullptr, u(key), u(nonce)) != 1 ||
        EVP_DecryptUpdate(c.get(), reinterpret_cast<unsigned char*>(pt.data()), &len, u(ct), static_cast<int>(ct.size())) != 1 ||
        EVP_CIPHER_CTX_ctrl(c.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(kTag), const_cast<char*>(tag.data())) != 1 ||
        EVP_DecryptFinal_ex(c.get(), reinterpret_cast<unsigned char*>(pt.data()) + len, &fin) != 1)
        return std::nullopt;   // a wrong key and a forged blob look the same: the tag does not verify
    pt.resize(static_cast<size_t>(len + fin));
    return pt;
}

}  // namespace crypto
}  // namespace gateway
}  // namespace dd
