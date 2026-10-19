#pragma once
#include <string>
#include <optional>

namespace dd {
namespace agent {

// Faithful port of agent/prompt_cache_boundary.py — a process-local LRU registry
// of builder-declared stable prefixes.
void register_stable_prefix(const std::string& prefix);
// Returns the longest registered prefix that is a *proper* prefix of content
// (len(content) > len(prefix)); nullopt if none.
std::optional<std::string> find_stable_prefix(const std::string& content);
void clear_stable_prefixes();

}  // namespace agent
}  // namespace dd
