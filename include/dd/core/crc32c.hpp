#pragma once
// dd/core/crc32c.hpp — CRC-32C (Castagnoli): SSE4.2 kernel when the CPU has it, a table otherwise.
//
// Chainable: crc32c(b, nb, crc32c(a, na)) == crc32c(a+b). The set is picked once per process by
// CPUID (see know/scan.hpp); crc32c_force() lets a test run the other one.

#include <cstddef>
#include <cstdint>

namespace dd {
namespace core {

uint32_t crc32c(const void* data, size_t n, uint32_t crc = 0);
uint32_t crc32c_table(const void* data, size_t n, uint32_t crc = 0);   // the portable reference
const char* crc32c_level();                                            // "sse4.2" or "table"
bool crc32c_force(const char* level);                                  // false (no change) when the CPU lacks it

}  // namespace core
}  // namespace dd
