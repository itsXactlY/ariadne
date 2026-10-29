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

// The host part of a URL as a client connects to it: userinfo (up to the last '@') and port stripped, IPv6 brackets
// removed, lower-cased, trailing dots dropped; "" when there is no usable host.
std::string url_hostname(const std::string& url);

// Full URL safety check: parse the URL, reject blocked hostnames, resolve
// the hostname via getaddrinfo, and block if any resolved address is blocked.
// Returns false on any parse/DNS failure (mirrors the Python try/except).
bool is_safe_url(const std::string& url);

// The hosts the operator wrote into the config -- the model endpoint, the MCP servers -- are the
// harness's own servers, the way loopback is on a desktop; on a phone the model lives on the LAN.
// admit_configured_url() adds a URL's host to that set and is_safe_url() then passes it without the
// private-range check. Only a configured URL is ever admitted, never one a tool argument carries, and
// the metadata hostnames stay refused. Boot only: one thread publishes, before any client runs.
void admit_configured_url(const std::string& url);
}  // namespace tools
}  // namespace dd
#endif
