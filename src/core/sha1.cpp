// Core SHA-1 + UUID5 primitives.
#include "dd/core/sha1.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dd {
namespace core {

static std::array<unsigned char, 20> sha1_block(const std::vector<unsigned char>& data) {
    std::uint32_t h0 = 0x67452301u, h1 = 0xEFCDAB89u, h2 = 0x98BADCFEu, h3 = 0x10325476u, h4 = 0xC3D2E1F0u;
    for (std::size_t off = 0; off < data.size(); off += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(data[off + i * 4]) << 24) |
                   (static_cast<std::uint32_t>(data[off + i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(data[off + i * 4 + 2]) << 8) |
                   (static_cast<std::uint32_t>(data[off + i * 4 + 3]));
        }
        for (int i = 16; i < 80; ++i) {
            std::uint32_t t = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = (t << 1) | (t >> 31);
        }
        std::uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            std::uint32_t tmp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d;
            d = c;
            c = (b << 30) | (b >> 2);
            b = a;
            a = tmp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }
    std::array<unsigned char, 20> out{};
    const std::uint32_t hs[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i) {
        out[i * 4 + 0] = static_cast<unsigned char>((hs[i] >> 24) & 0xFF);
        out[i * 4 + 1] = static_cast<unsigned char>((hs[i] >> 16) & 0xFF);
        out[i * 4 + 2] = static_cast<unsigned char>((hs[i] >> 8) & 0xFF);
        out[i * 4 + 3] = static_cast<unsigned char>(hs[i] & 0xFF);
    }
    return out;
}

static std::array<unsigned char, 20> sha1_bytes(const std::string& msg) {
    std::vector<unsigned char> data(msg.begin(), msg.end());
    const std::uint64_t bitlen = static_cast<std::uint64_t>(data.size()) * 8ULL;
    data.push_back(0x80);
    while (data.size() % 64 != 56) data.push_back(0);
    for (int i = 7; i >= 0; --i) data.push_back(static_cast<unsigned char>((bitlen >> (i * 8)) & 0xFF));
    return sha1_block(data);
}

static std::string to_hex(const unsigned char* p, std::size_t n) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(digits[p[i] >> 4]);
        out.push_back(digits[p[i] & 0x0F]);
    }
    return out;
}

std::string sha1_hex(const std::string& data) {
    std::array<unsigned char, 20> h = sha1_bytes(data);
    return to_hex(h.data(), h.size());
}

std::string uuid5_url_hex(const std::string& name) {
    // NAMESPACE_URL = 6ba7b811-9dad-11d1-80b4-00c04fd430c8
    static const unsigned char ns[16] = {0x6b, 0xa7, 0xb8, 0x11, 0x9d, 0xad, 0x11, 0xd1,
                                         0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8};
    std::string combined;
    combined.append(reinterpret_cast<const char*>(ns), 16);
    combined += name;
    std::array<unsigned char, 20> h = sha1_bytes(combined);
    h[6] = static_cast<unsigned char>((h[6] & 0x0F) | 0x50);  // version 5
    h[8] = static_cast<unsigned char>((h[8] & 0x3F) | 0x80);  // variant 10
    return to_hex(h.data(), 16);
}

}  // namespace core
}  // namespace dd
