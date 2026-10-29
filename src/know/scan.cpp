// src/know/scan.cpp — impl of dd/know/scan.hpp. The kernel exists three times; the AVX
// ones carry their ISA in a target attribute, so the TU builds with the project's
// baseline flags and dispatch decides at run time.
#include "dd/know/scan.hpp"

#if defined(__x86_64__)
#include <immintrin.h>
#define DD_SCAN_X86 1
#endif

#include <atomic>
#include <cstring>
#include <initializer_list>

namespace dd {
namespace know {
namespace simd {

namespace {

// ---- scalar ----
void classify_scalar(const char* s, size_t n, Masks* m) {
    Masks r{};
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const uint64_t bit = uint64_t{1} << i;
        if (c >= 'A' && c <= 'Z') r.upper |= bit, r.alnum |= bit;
        else if (c >= 'a' && c <= 'z') r.lower |= bit, r.alnum |= bit;
        else if (c >= '0' && c <= '9') r.alnum |= bit;
        else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') r.space |= bit;
        else if (c >= 0x80) r.high |= bit;
    }
    *m = r;
}

void classify32_scalar(const char* s, size_t n, Masks32* m) {
    Masks w;
    classify_scalar(s, n, &w);
    *m = {static_cast<uint32_t>(w.alnum), static_cast<uint32_t>(w.upper), static_cast<uint32_t>(w.lower), static_cast<uint32_t>(w.high)};
}

#ifdef DD_SCAN_X86
// ---- AVX2 ----
__attribute__((target("avx2"))) inline __m256i in_range(__m256i v, char lo, char hi) {   // lo <= v <= hi, unsigned
    const __m256i d = _mm256_sub_epi8(v, _mm256_set1_epi8(lo));
    return _mm256_cmpeq_epi8(_mm256_min_epu8(d, _mm256_set1_epi8(static_cast<char>(hi - lo))), d);
}

__attribute__((target("avx2"))) inline uint64_t bits(__m256i v) { return static_cast<uint32_t>(_mm256_movemask_epi8(v)); }

__attribute__((target("avx2"))) void classify_avx2(const char* s, size_t n, Masks* m) {
    alignas(32) char tail[64];
    if (n < 64) {   // never read past the text: the zero bytes after it are in no class
        std::memset(tail, 0, sizeof tail);
        std::memcpy(tail, s, n);
        s = tail;
    }
    Masks r{};
    for (int half = 0; half < 2; ++half) {
        const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + 32 * half));
        const __m256i up = in_range(v, 'A', 'Z'), lo = in_range(v, 'a', 'z'), dg = in_range(v, '0', '9');
        __m256i sp = _mm256_cmpeq_epi8(v, _mm256_set1_epi8(' '));
        sp = _mm256_or_si256(sp, _mm256_cmpeq_epi8(v, _mm256_set1_epi8('\t')));
        sp = _mm256_or_si256(sp, _mm256_cmpeq_epi8(v, _mm256_set1_epi8('\n')));
        sp = _mm256_or_si256(sp, _mm256_cmpeq_epi8(v, _mm256_set1_epi8('\r')));
        const int sh = 32 * half;
        r.upper |= bits(up) << sh;
        r.lower |= bits(lo) << sh;
        r.alnum |= bits(_mm256_or_si256(_mm256_or_si256(up, lo), dg)) << sh;
        r.space |= bits(sp) << sh;
        r.high |= bits(v) << sh;   // movemask takes each byte's top bit: >= 0x80
    }
    *m = r;
}

__attribute__((target("avx2"))) void classify32_avx2(const char* s, size_t n, Masks32* m) {
    alignas(32) char tail[32];
    if (n < 32) {
        std::memset(tail, 0, sizeof tail);
        std::memcpy(tail, s, n);
        s = tail;
    }
    const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s));
    const __m256i up = in_range(v, 'A', 'Z'), lo = in_range(v, 'a', 'z'), dg = in_range(v, '0', '9');
    m->upper = static_cast<uint32_t>(bits(up));
    m->lower = static_cast<uint32_t>(bits(lo));
    m->alnum = static_cast<uint32_t>(bits(_mm256_or_si256(_mm256_or_si256(up, lo), dg)));
    m->high = static_cast<uint32_t>(bits(v));
}

// ---- AVX-512 (F + BW) ----
__attribute__((target("avx512f,avx512bw,bmi2"))) void classify_avx512(const char* s, size_t n, Masks* m) {
    const __mmask64 live = n >= 64 ? ~0ull : _bzhi_u64(~0ull, static_cast<unsigned>(n));   // masked load: no overread
    const __m512i v = _mm512_maskz_loadu_epi8(live, s);
    const __mmask64 up = _mm512_cmple_epu8_mask(_mm512_sub_epi8(v, _mm512_set1_epi8('A')), _mm512_set1_epi8('Z' - 'A'));
    const __mmask64 lo = _mm512_cmple_epu8_mask(_mm512_sub_epi8(v, _mm512_set1_epi8('a')), _mm512_set1_epi8('z' - 'a'));
    const __mmask64 dg = _mm512_cmple_epu8_mask(_mm512_sub_epi8(v, _mm512_set1_epi8('0')), _mm512_set1_epi8('9' - '0'));
    const __mmask64 sp = _mm512_cmpeq_epi8_mask(v, _mm512_set1_epi8(' ')) | _mm512_cmpeq_epi8_mask(v, _mm512_set1_epi8('\t')) |
                         _mm512_cmpeq_epi8_mask(v, _mm512_set1_epi8('\n')) | _mm512_cmpeq_epi8_mask(v, _mm512_set1_epi8('\r'));
    m->upper = up;
    m->lower = lo;
    m->alnum = up | lo | dg;
    m->space = sp;
    m->high = _mm512_movepi8_mask(v);
}

__attribute__((target("avx512f,avx512bw,bmi2"))) void classify32_avx512(const char* s, size_t n, Masks32* m) {
    Masks w;
    classify_avx512(s, n, &w);   // n <= 32: nothing above bit 31
    *m = {static_cast<uint32_t>(w.alnum), static_cast<uint32_t>(w.upper), static_cast<uint32_t>(w.lower), static_cast<uint32_t>(w.high)};
}
#endif

struct Kernels {
    const char* name;
    void (*classify)(const char*, size_t, Masks*);
    void (*classify32)(const char*, size_t, Masks32*);
};

constexpr Kernels kScalar{"scalar", classify_scalar, classify32_scalar};
#ifdef DD_SCAN_X86
constexpr Kernels kAvx2{"avx2", classify_avx2, classify32_avx2};
constexpr Kernels kAvx512{"avx512", classify_avx512, classify32_avx512};
#else
// No x86 vectors on this target (aarch64 Android): the names stay so level() and force() answer
// the same way everywhere, the bodies are scalar, and has() never picks them.
constexpr Kernels kAvx2{"avx2", classify_scalar, classify32_scalar};
constexpr Kernels kAvx512{"avx512", classify_scalar, classify32_scalar};
#endif

bool has(const Kernels& k) {
#ifdef DD_SCAN_X86
    __builtin_cpu_init();
    if (&k == &kAvx512) return __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") && __builtin_cpu_supports("bmi2");
    if (&k == &kAvx2) return __builtin_cpu_supports("avx2");
#else
    if (&k != &kScalar) return false;
#endif
    return true;
}

const Kernels* best() {
    if (has(kAvx512)) return &kAvx512;
    if (has(kAvx2)) return &kAvx2;
    return &kScalar;
}

// First use, not static init: a static initializer elsewhere may tokenize.
std::atomic<const Kernels*>& slot() {
    static std::atomic<const Kernels*> g{best()};
    return g;
}

inline const Kernels& k() { return *slot().load(std::memory_order_relaxed); }

}  // namespace

const char* level() { return k().name; }

bool force(const char* name) {
    for (const Kernels* c : {&kAvx512, &kAvx2, &kScalar})
        if (std::strcmp(c->name, name) == 0 && has(*c)) {
            slot().store(c, std::memory_order_relaxed);
            return true;
        }
    return false;
}

void classify(const char* s, size_t n, Masks* m) { k().classify(s, n, m); }
void classify32(const char* s, size_t n, Masks32* m) { k().classify32(s, n, m); }

}  // namespace simd
}  // namespace know
}  // namespace dd
