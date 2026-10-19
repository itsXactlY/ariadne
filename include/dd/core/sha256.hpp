// Core SHA-256 primitive (hashlib.sha256 equivalent).
#ifndef DD_CORE_SHA256_HPP
#define DD_CORE_SHA256_HPP
#include <string>
namespace dd {
namespace core {
// Lowercase hex digest of SHA-256 over the raw bytes (64 hex chars).
std::string sha256_hex(const std::string& data);
}  // namespace core
}  // namespace dd
#endif
