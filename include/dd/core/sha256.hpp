// Core SHA-256 primitive (hashlib.sha256 equivalent).
#ifndef DD_CORE_SHA256_HPP
#define DD_CORE_SHA256_HPP
#include <string>
namespace dd {
namespace core {
// Lowercase hex digest of SHA-256 over the raw bytes (64 hex chars).
std::string sha256_hex(const std::string& data);

namespace sha256 {
// The compress kernel in use: "shani" (x86 SHA extensions) or "scalar", picked once per process by CPUID.
const char* level();
// Tests: switch kernels; false (and no change) when the CPU lacks it.
bool force(const char* level);
}  // namespace sha256
}  // namespace core
}  // namespace dd
#endif
