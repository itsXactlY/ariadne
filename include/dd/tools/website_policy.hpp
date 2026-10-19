#pragma once
// Pure host/URL policy helpers ported from tools/website_policy.py.
// YAML config load + threading cache + check_website_access are NOT ported
// (fs/threading coupled); only the four pure host helpers below are claimed.
#include "dd/core/json.hpp"
#include <string>
#include <optional>

namespace dd {
namespace tools {

// (host or "").strip().lower().rstrip(".")
std::string normalize_host(const std::string& host);

// Returns nullopt when the rule is not a string or normalizes to empty /
// comment (mirrors Optional[str]).
std::optional<std::string> normalize_rule(const core::Json& rule);

// host/pattern already normalized by callers; glob `*`/`?`/`[...]` for `*.` patterns.
bool match_host_against_rule(const std::string& host, const std::string& pattern);

std::string extract_host_from_urlish(const std::string& url);

}  // namespace tools
}  // namespace dd
