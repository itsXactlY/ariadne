// src/ui_simd.cpp — impl of dd/ui_simd.hpp. Each kernel exists three times;
// the AVX ones carry their ISA in a target attribute, so the TU builds with
// the project's baseline flags and dispatch decides at run time.
#include "dd/ui_simd.hpp"

#if defined(__x86_64__)
#include <immintrin.h>
#define DD_UI_X86 1
#endif

#include <atomic>
#include <cstring>

namespace dd {
namespace ui {
namespace simd {

namespace {

#ifdef DD_UI_X86
// Four "cell differs" nibbles (bits 0,4,8,12 after the fold) -> 4 dirty bytes.
constexpr uint32_t kNibbleBytes[16] = {
    0x00000000, 0x00000001, 0x00000100, 0x00000101, 0x00010000, 0x00010001, 0x00010100, 0x00010101,
    0x01000000, 0x01000001, 0x01000100, 0x01000101, 0x01010000, 0x01010001, 0x01010100, 0x01010101};

inline unsigned fold4(unsigned m) {   // 16 dword-mismatch bits -> one bit per cell
    m |= m >> 1;
    m |= m >> 2;
    return (m & 1u) | ((m >> 3) & 2u) | ((m >> 6) & 4u) | ((m >> 9) & 8u);
}
#endif

// ---- scalar ----
int diff_scalar(const Cell* a, const Cell* b, int n, char* dirty) {
    int cnt = 0;
    for (int i = 0; i < n; ++i) {
        const char d = std::memcmp(a + i, b + i, sizeof(Cell)) != 0;
        dirty[i] = d;
        cnt += d;
    }
    return cnt;
}

size_t run_scalar(const char* s, size_t n) {
    size_t i = 0;
    while (i < n && static_cast<unsigned char>(s[i]) < 0x80) ++i;
    return i;
}

size_t printable_scalar(const char* s, size_t n) {
    size_t c = 0;
    for (size_t i = 0; i < n; ++i) c += s[i] >= 0x20 && s[i] < 0x7F;
    return c;
}

void widen_scalar(const char* s, size_t n, char32_t* out) {
    for (size_t i = 0; i < n; ++i) out[i] = static_cast<unsigned char>(s[i]);
}

#ifdef DD_UI_X86
// ---- AVX2 ----
__attribute__((target("avx2,popcnt"))) int diff_avx2(const Cell* a, const Cell* b, int n, char* dirty) {
    int cnt = 0, i = 0;
    for (; i + 4 <= n; i += 4) {   // 4 cells = two 256-bit lanes
        const __m256i a0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i));
        const __m256i b0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i));
        const __m256i a1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i + 2));
        const __m256i b1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i + 2));
        const unsigned eq0 = static_cast<unsigned>(_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(a0, b0))));
        const unsigned eq1 = static_cast<unsigned>(_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(a1, b1))));
        const unsigned cells = fold4(~(eq0 | (eq1 << 8)) & 0xFFFFu);
        std::memcpy(dirty + i, &kNibbleBytes[cells], 4);
        cnt += _mm_popcnt_u32(cells);
    }
    return cnt + diff_scalar(a + i, b + i, n - i, dirty + i);
}

__attribute__((target("avx2,bmi"))) size_t run_avx2(const char* s, size_t n) {
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        const unsigned m = static_cast<unsigned>(_mm256_movemask_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + i))));
        if (m) return i + _tzcnt_u32(m);
    }
    return i + run_scalar(s + i, n - i);
}

__attribute__((target("avx2,popcnt"))) size_t printable_avx2(const char* s, size_t n) {
    const __m256i lo = _mm256_set1_epi8(0x1F), hi = _mm256_set1_epi8(0x7F);
    size_t c = 0, i = 0;
    for (; i + 32 <= n; i += 32) {   // ASCII only, so signed compares are exact
        const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + i));
        const __m256i ok = _mm256_and_si256(_mm256_cmpgt_epi8(v, lo), _mm256_cmpgt_epi8(hi, v));
        c += static_cast<size_t>(_mm_popcnt_u32(static_cast<unsigned>(_mm256_movemask_epi8(ok))));
    }
    return c + printable_scalar(s + i, n - i);
}

__attribute__((target("avx2"))) void widen_avx2(const char* s, size_t n, char32_t* out) {
    size_t i = 0;
    for (; i + 8 <= n; i += 8)
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + i),
                            _mm256_cvtepu8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(s + i))));
    widen_scalar(s + i, n - i, out + i);
}

// ---- AVX-512 (F + BW) ----
__attribute__((target("avx512f,avx512bw,popcnt"))) int diff_avx512(const Cell* a, const Cell* b, int n, char* dirty) {
    int cnt = 0, i = 0;
    for (; i + 16 <= n; i += 16) {   // 16 cells = four 512-bit lanes
#pragma GCC unroll 4
        for (int k = 0; k < 4; ++k) {
            const __m512i va = _mm512_loadu_si512(a + i + 4 * k);
            const __m512i vb = _mm512_loadu_si512(b + i + 4 * k);
            const unsigned cells = fold4(_mm512_cmpneq_epi32_mask(va, vb));
            std::memcpy(dirty + i + 4 * k, &kNibbleBytes[cells], 4);
            cnt += _mm_popcnt_u32(cells);
        }
    }
    for (; i + 4 <= n; i += 4) {
        const unsigned cells = fold4(_mm512_cmpneq_epi32_mask(_mm512_loadu_si512(a + i), _mm512_loadu_si512(b + i)));
        std::memcpy(dirty + i, &kNibbleBytes[cells], 4);
        cnt += _mm_popcnt_u32(cells);
    }
    return cnt + diff_scalar(a + i, b + i, n - i, dirty + i);
}

__attribute__((target("avx512f,avx512bw,bmi,bmi2"))) size_t run_avx512(const char* s, size_t n) {
    size_t i = 0;
    for (; i + 64 <= n; i += 64) {
        const uint64_t m = _mm512_movepi8_mask(_mm512_loadu_si512(s + i));
        if (m) return i + _tzcnt_u64(m);
    }
    if (i < n) {   // masked tail: no scalar loop
        const __mmask64 live = _bzhi_u64(~0ull, static_cast<unsigned>(n - i));
        const uint64_t m = _mm512_movepi8_mask(_mm512_maskz_loadu_epi8(live, s + i)) & live;
        return m ? i + _tzcnt_u64(m) : n;
    }
    return i;
}

__attribute__((target("avx512f,avx512bw,popcnt,bmi2"))) size_t printable_avx512(const char* s, size_t n) {
    const __m512i lo = _mm512_set1_epi8(0x20), hi = _mm512_set1_epi8(0x7F);
    size_t c = 0, i = 0;
    for (; i < n; i += 64) {
        const __mmask64 live = n - i >= 64 ? ~0ull : _bzhi_u64(~0ull, static_cast<unsigned>(n - i));
        const __m512i v = _mm512_maskz_loadu_epi8(live, s + i);
        const __mmask64 ok = _mm512_mask_cmpge_epu8_mask(live, v, lo) & _mm512_cmplt_epu8_mask(v, hi);
        c += static_cast<size_t>(_mm_popcnt_u64(ok));
    }
    return c;
}

__attribute__((target("avx512f,avx512bw,avx512vl,bmi2"))) void widen_avx512(const char* s, size_t n, char32_t* out) {
    size_t i = 0;
    for (; i + 16 <= n; i += 16)
        _mm512_storeu_si512(out + i, _mm512_cvtepu8_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(s + i))));
    if (i < n) {
        const __mmask16 live = static_cast<__mmask16>(_bzhi_u32(0xFFFFu, static_cast<unsigned>(n - i)));
        const __m128i v = _mm_maskz_loadu_epi8(live, s + i);
        _mm512_mask_storeu_epi32(out + i, live, _mm512_cvtepu8_epi32(v));
    }
}
#endif

struct Kernels {
    const char* name;
    int (*diff)(const Cell*, const Cell*, int, char*);
    size_t (*run)(const char*, size_t);
    size_t (*printable)(const char*, size_t);
    void (*widen)(const char*, size_t, char32_t*);
};

constexpr Kernels kScalar{"scalar", diff_scalar, run_scalar, printable_scalar, widen_scalar};
#ifdef DD_UI_X86
constexpr Kernels kAvx2{"avx2", diff_avx2, run_avx2, printable_avx2, widen_avx2};
constexpr Kernels kAvx512{"avx512", diff_avx512, run_avx512, printable_avx512, widen_avx512};
#else
// No x86 vectors on this target: the names stay, the bodies are scalar, has() never picks them.
constexpr Kernels kAvx2{"avx2", diff_scalar, run_scalar, printable_scalar, widen_scalar};
constexpr Kernels kAvx512{"avx512", diff_scalar, run_scalar, printable_scalar, widen_scalar};
#endif

bool has(const Kernels& k) {
#ifdef DD_UI_X86
    __builtin_cpu_init();
    if (&k == &kAvx512)
        return __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") &&
               __builtin_cpu_supports("avx512vl") && __builtin_cpu_supports("bmi2");
    if (&k == &kAvx2) return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("bmi");
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

// First use, not static init: another TU's static initializer may measure text (see know/scan.cpp).
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

int diff_cells(const Cell* a, const Cell* b, int n, char* dirty) { return k().diff(a, b, n, dirty); }
size_t ascii_run(const char* s, size_t n) { return k().run(s, n); }
size_t ascii_printable(const char* s, size_t n) { return k().printable(s, n); }
void widen_ascii(const char* s, size_t n, char32_t* out) { k().widen(s, n, out); }

}  // namespace simd
}  // namespace ui
}  // namespace dd
