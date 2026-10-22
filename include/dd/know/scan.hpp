#pragma once
// dd/know/scan.hpp — the tokenizer's byte classification as AVX-512 / AVX2 / scalar
// kernels, picked once per process by CPUID (__builtin_cpu_supports), so one binary
// runs everywhere and uses the widest unit the CPU has (see ui_simd.hpp).

#include <cstddef>
#include <cstdint>

namespace dd {
namespace know {
namespace simd {

// Up to 64 bytes of text as bit masks (bit i = byte i). Bytes at or past n are nothing.
struct Masks {
    uint64_t alnum;   // 0-9 A-Z a-z
    uint64_t upper;   // A-Z
    uint64_t lower;   // a-z
    uint64_t space;   // ' ' \t \n \r
    uint64_t high;    // >= 0x80: part of a multi-byte character
};

// "avx512", "avx2" or "scalar": the set in use.
const char* level();
// Tests: switch to a set; false (and no change) when the CPU lacks it.
bool force(const char* level);

// Classifies s[0..n), n <= 64. Never reads past s + n.
void classify(const char* s, size_t n, Masks* m);

// The same for one word: n <= 32, a single vector.
struct Masks32 {
    uint32_t alnum, upper, lower, high;
};
void classify32(const char* s, size_t n, Masks32* m);

}  // namespace simd
}  // namespace know
}  // namespace dd
