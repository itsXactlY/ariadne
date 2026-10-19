// Core SHA-1 + UUID5 primitives (uuid.uuid5 / hashlib.sha1 equivalents).
#ifndef DD_CORE_SHA1_HPP
#define DD_CORE_SHA1_HPP
#include <string>
namespace dd {
namespace core {
// Lowercase hex digest of SHA-1 over the raw bytes (40 hex chars).
std::string sha1_hex(const std::string& data);
// uuid.uuid5(uuid.NAMESPACE_URL, name).hex  (32 hex chars, version-5 bits set).
std::string uuid5_url_hex(const std::string& name);
}  // namespace core
}  // namespace dd
#endif
