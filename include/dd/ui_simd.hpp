#pragma once
// dd/ui_simd.hpp — the UI's hot loops as AVX-512 / AVX2 / scalar kernels,
// picked once per process by CPUID (__builtin_cpu_supports), so one binary
// runs everywhere and uses the widest unit the CPU has.

#include <cstddef>
#include <cstdint>

#include "dd/ui.hpp"

namespace dd {
namespace ui {
namespace simd {

// "avx512", "avx2" or "scalar": the set in use.
const char* level();
// Tests: switch to a set; false (and no change) when the CPU lacks it.
bool force(const char* level);

// dirty[i] = (a[i] != b[i]) for n cells; returns how many differ.
int diff_cells(const Cell* a, const Cell* b, int n, char* dirty);
// Length of the leading run of ASCII bytes (< 0x80), at most n.
size_t ascii_run(const char* s, size_t n);
// Printable ASCII (0x20..0x7E) bytes among s[0..n); s must be ASCII.
size_t ascii_printable(const char* s, size_t n);
// out[i] = s[i] for n ASCII bytes.
void widen_ascii(const char* s, size_t n, char32_t* out);

}  // namespace simd
}  // namespace ui
}  // namespace dd
