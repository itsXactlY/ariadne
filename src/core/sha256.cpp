// Core SHA-256 primitive (hashlib.sha256 / FIPS 180-2 equivalent). The block function exists twice,
// scalar and SHA-NI; the SHA-NI one carries its ISA in a target attribute, so the TU builds with the
// project's baseline flags and dispatch decides at run time (see ui_simd.cpp).
#include "dd/core/sha256.hpp"

#if defined(__x86_64__)
#include <immintrin.h>
#define DD_SHA_HW 1
#endif

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>

namespace dd {
namespace core {

namespace {

inline uint32_t rotr(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
}

const std::array<uint32_t, 8> IV = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

const std::array<uint32_t, 64> K = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

// ---- scalar ----
void compress_scalar(uint32_t h[8], const uint8_t* data, size_t blocks) {
    for (; blocks; --blocks, data += 64) {
        uint32_t w[64];
        for (int t = 0; t < 16; ++t) {
            w[t] = (static_cast<uint32_t>(data[4 * t]) << 24) |
                   (static_cast<uint32_t>(data[4 * t + 1]) << 16) |
                   (static_cast<uint32_t>(data[4 * t + 2]) << 8) |
                   static_cast<uint32_t>(data[4 * t + 3]);
        }
        for (int t = 16; t < 64; ++t) {
            uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
            uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
            w[t] = w[t - 16] + s0 + w[t - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int t = 0; t < 64; ++t) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t temp1 = hh + S1 + ch + K[t] + w[t];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = S0 + maj;
            hh = g; g = f; f = e; e = d + temp1; d = c; c = b; b = a; a = temp1 + temp2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
}

// ---- SHA-NI ----
#ifdef DD_SHA_HW
__attribute__((target("sha,sse4.1,ssse3"))) void compress_shani(uint32_t h[8], const uint8_t* data, size_t blocks) {
    const __m128i bswap = _mm_set_epi64x(0x0c0d0e0f08090a0bll, 0x0405060700010203ll);   // big-endian words
    // the state as the instructions want it: ABEF and CDGH
    __m128i tmp = _mm_shuffle_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(&h[0])), 0xB1);   // CDAB
    __m128i s1 = _mm_shuffle_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(&h[4])), 0x1B);    // EFGH
    __m128i s0 = _mm_alignr_epi8(tmp, s1, 8);       // ABEF
    s1 = _mm_blend_epi16(s1, tmp, 0xF0);            // CDGH
    for (; blocks; --blocks, data += 64) {
        const __m128i abef = s0, cdgh = s1;
        __m128i m[4];   // four message words each; a ring over the schedule
        for (int i = 0; i < 4; ++i) m[i] = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 16 * i)), bswap);
#pragma GCC unroll 16
        for (int g = 0; g < 16; ++g) {   // four rounds a group
            if (g >= 4) {                // W[g] = msg2(msg1(W[g-4], W[g-3]) + (W[g-1]:W[g-2] >> 4 bytes), W[g-1])
                const __m128i w4 = m[g & 3], w3 = m[(g + 1) & 3], w2 = m[(g + 2) & 3], w1 = m[(g + 3) & 3];
                m[g & 3] = _mm_sha256msg2_epu32(_mm_add_epi32(_mm_sha256msg1_epu32(w4, w3), _mm_alignr_epi8(w1, w2, 4)), w1);
            }
            __m128i msg = _mm_add_epi32(m[g & 3], _mm_loadu_si128(reinterpret_cast<const __m128i*>(&K[4 * static_cast<size_t>(g)])));
            s1 = _mm_sha256rnds2_epu32(s1, s0, msg);
            msg = _mm_shuffle_epi32(msg, 0x0E);
            s0 = _mm_sha256rnds2_epu32(s0, s1, msg);
        }
        s0 = _mm_add_epi32(s0, abef);
        s1 = _mm_add_epi32(s1, cdgh);
    }
    tmp = _mm_shuffle_epi32(s0, 0x1B);   // FEBA
    s1 = _mm_shuffle_epi32(s1, 0xB1);    // DCHG
    _mm_storeu_si128(reinterpret_cast<__m128i*>(&h[0]), _mm_blend_epi16(tmp, s1, 0xF0));   // DCBA
    _mm_storeu_si128(reinterpret_cast<__m128i*>(&h[4]), _mm_alignr_epi8(s1, tmp, 8));      // HGFE
}
#else
// No SHA-NI off x86: the kernel keeps its name, the body is the scalar one, and has() says no.
void compress_shani(uint32_t h[8], const uint8_t* data, size_t blocks) { compress_scalar(h, data, blocks); }
#endif

struct Kernel {
    const char* name;
    void (*compress)(uint32_t*, const uint8_t*, size_t);
};

constexpr Kernel kScalar{"scalar", compress_scalar};
constexpr Kernel kShani{"shani", compress_shani};

bool has(const Kernel& k) {
#ifdef DD_SHA_HW
    __builtin_cpu_init();
    if (&k == &kShani) return __builtin_cpu_supports("sha") && __builtin_cpu_supports("sse4.1") && __builtin_cpu_supports("ssse3");
#else
    if (&k == &kShani) return false;
#endif
    return true;
}

const Kernel* best() { return has(kShani) ? &kShani : &kScalar; }

// First use, not static init: another TU's static initializer may hash.
std::atomic<const Kernel*>& slot() {
    static std::atomic<const Kernel*> g{best()};
    return g;
}

inline const Kernel& kernel() { return *slot().load(std::memory_order_relaxed); }

}  // namespace

namespace sha256 {

const char* level() { return kernel().name; }

bool force(const char* name) {
    for (const Kernel* k : {&kShani, &kScalar})
        if (std::strcmp(k->name, name) == 0 && has(*k)) {
            slot().store(k, std::memory_order_relaxed);
            return true;
        }
    return false;
}

}  // namespace sha256

std::string sha256_hex(const std::string& data) {
    uint32_t h[8];
    for (int i = 0; i < 8; ++i) h[i] = IV[static_cast<size_t>(i)];
    const Kernel& k = kernel();

    // The whole blocks straight from the input; only the padded tail (one or two blocks) is built.
    const uint8_t* p = reinterpret_cast<const uint8_t*>(data.data());
    const size_t n = data.size();
    const size_t full = n / 64, rem = n - full * 64;
    if (full) k.compress(h, p, full);

    uint8_t tail[128] = {0};   // data + 0x80 + zeros + 8-byte big-endian bit length
    if (rem) std::memcpy(tail, p + full * 64, rem);
    tail[rem] = 0x80;
    const size_t tail_blocks = rem < 56 ? 1 : 2;
    const uint64_t bitlen = static_cast<uint64_t>(n) * 8ull;
    for (size_t i = 0; i < 8; ++i) tail[tail_blocks * 64 - 1 - i] = static_cast<uint8_t>((bitlen >> (8 * i)) & 0xFF);
    k.compress(h, tail, tail_blocks);

    static const char* hexd = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (int i = 0; i < 8; ++i) {
        for (int sh = 24; sh >= 0; sh -= 8) {
            unsigned byte = (h[i] >> sh) & 0xFFu;
            out.push_back(hexd[(byte >> 4) & 0x0F]);
            out.push_back(hexd[byte & 0x0F]);
        }
    }
    return out;
}

}  // namespace core
}  // namespace dd
