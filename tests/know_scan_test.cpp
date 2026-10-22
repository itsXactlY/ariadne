// dd::know::simd — each kernel set (scalar, AVX2, AVX-512 where the CPU has it) classifies every byte
// and every length like a plain loop, and never reads past the text it was given.
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <random>
#include <string>

#include "dd/know/scan.hpp"

using namespace dd::know;

static int g_fail = 0;
static void chk(bool ok, const std::string& label) {
    if (!ok) {
        ++g_fail;
        std::printf("FAIL %s\n", label.c_str());
    }
}

// the definition, written plainly and apart from the kernels
static simd::Masks reference(const char* s, size_t n) {
    simd::Masks r{};
    for (size_t i = 0; i < n; ++i) {
        const unsigned c = static_cast<unsigned char>(s[i]);
        const uint64_t bit = uint64_t{1} << i;
        const bool up = c >= 'A' && c <= 'Z', lo = c >= 'a' && c <= 'z', dg = c >= '0' && c <= '9';
        if (up) r.upper |= bit;
        if (lo) r.lower |= bit;
        if (up || lo || dg) r.alnum |= bit;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') r.space |= bit;
        if (c >= 0x80) r.high |= bit;
    }
    return r;
}

static bool same32(const simd::Masks32& a, const simd::Masks& r) {
    return a.alnum == static_cast<uint32_t>(r.alnum) && a.upper == static_cast<uint32_t>(r.upper) && a.lower == static_cast<uint32_t>(r.lower) &&
           a.high == static_cast<uint32_t>(r.high);
}

static bool same(const simd::Masks& a, const simd::Masks& b) {
    return a.alnum == b.alnum && a.upper == b.upper && a.lower == b.lower && a.space == b.space && a.high == b.high;
}

static void check_level(const char* level) {
    if (!simd::force(level)) {
        std::printf("skip %s (cpu lacks it)\n", level);
        return;
    }
    std::printf("kernels: %s\n", simd::level());
    const std::string tag = level;

    // every byte value, at the lengths that straddle a vector
    for (int b = 0; b < 256; ++b) {
        const std::string buf(64, static_cast<char>(b));
        for (size_t n : {size_t{0}, size_t{1}, size_t{2}, size_t{31}, size_t{32}, size_t{33}, size_t{63}, size_t{64}}) {
            simd::Masks got{};
            simd::classify(buf.data(), n, &got);
            chk(same(got, reference(buf.data(), n)), tag + " byte " + std::to_string(b) + " n " + std::to_string(n));
            if (n <= 32) {
                simd::Masks32 got32{};
                simd::classify32(buf.data(), n, &got32);
                chk(same32(got32, reference(buf.data(), n)), tag + " byte32 " + std::to_string(b) + " n " + std::to_string(n));
            }
        }
    }

    // random text, biased toward what the tokenizer meets
    std::mt19937 rng(12345);
    const std::string alphabet = "abcxyzABCXYZ0189 \t\n\r.,;:_-()/\"'@#";
    for (int it = 0; it < 60000; ++it) {
        char buf[64];
        const size_t n = rng() % 65;
        for (size_t i = 0; i < n; ++i) buf[i] = rng() % 7 == 0 ? static_cast<char>(rng() % 256) : alphabet[rng() % alphabet.size()];
        simd::Masks got{};
        simd::classify(buf, n, &got);
        if (!same(got, reference(buf, n))) {
            chk(false, tag + " random case " + std::to_string(it) + " n " + std::to_string(n));
            break;
        }
        if (n <= 32) {
            simd::Masks32 got32{};
            simd::classify32(buf, n, &got32);
            if (!same32(got32, reference(buf, n))) {
                chk(false, tag + " random32 case " + std::to_string(it) + " n " + std::to_string(n));
                break;
            }
        }
    }

    // never past the text: the bytes after it are a guard page
    const long page = sysconf(_SC_PAGESIZE);
    char* base = static_cast<char*>(mmap(nullptr, 2 * static_cast<size_t>(page), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    chk(base != MAP_FAILED, "mmap");
    if (base != MAP_FAILED) {
        mprotect(base + page, static_cast<size_t>(page), PROT_NONE);
        for (size_t n = 0; n <= 64; ++n) {
            char* p = base + page - n;
            for (size_t i = 0; i < n; ++i) p[i] = "aZ9 \xc3\xa4"[i % 6];
            simd::Masks got{};
            simd::classify(p, n, &got);   // a read past p + n would fault here
            chk(same(got, reference(p, n)), tag + " guard n " + std::to_string(n));
            if (n <= 32) {
                simd::Masks32 got32{};
                simd::classify32(p, n, &got32);   // ... and here
                chk(same32(got32, reference(p, n)), tag + " guard32 n " + std::to_string(n));
            }
        }
        munmap(base, 2 * static_cast<size_t>(page));
    }
}

int main() {
    const std::string best = simd::level();
    check_level("scalar");
    check_level("avx2");
    check_level("avx512");
    chk(!simd::force("sse9"), "an unknown level is refused");
    simd::force(best.c_str());
    chk(best == simd::level(), "the best level is restored");
    std::printf("best level on this CPU: %s\n", simd::level());
    if (g_fail) std::printf("%d FAILED\n", g_fail);
    else std::printf("all know scan tests passed\n");
    return g_fail ? 1 : 0;
}
