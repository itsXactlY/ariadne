#ifndef DD_TOOLS_URL_SAFETY_HPP
#define DD_TOOLS_URL_SAFETY_HPP
// Port of tools/url_safety.py — SSRF gate for outbound fetches.
#include <string>
namespace dd {
namespace tools {
// Parse an IPv4/IPv6 textual address and classify it against the
// ipaddress special-purpose registries (private/loopback/link-local/
// reserved/multicast/unspecified) plus the CGNAT 100.64.0.0/10 block.
// Returns true if the address must be blocked. Throws std::invalid_argument
// if the string is not a valid IPv4 or IPv6 address.
bool is_blocked_ip(const std::string& ip_str);

// Full URL safety check: parse the URL, reject blocked hostnames, resolve
// the hostname via getaddrinfo, and block if any resolved address is blocked.
// Returns false on any parse/DNS failure (mirrors the Python try/except).
bool is_safe_url(const std::string& url);
}  // namespace tools
}  // namespace dd
#endif
