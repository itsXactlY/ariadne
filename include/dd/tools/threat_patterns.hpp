#pragma once
// dd::tools::threat_patterns — shared threat-pattern library for context-window
// security scanning. Port of tools/threat_patterns.py.
#include <string>
#include <vector>
#include <optional>

namespace dd {
namespace tools {

// Return matched pattern IDs in `content` at the given scope ("all"/"context"/"strict").
// Invisible-unicode hits are reported first as "invisible_unicode_U+XXXX".
// Throws std::runtime_error for an unknown scope (mirrors the Python ValueError).
std::vector<std::string> scan_for_threats(const std::string& content,
                                          const std::string& scope = "context");

// Human-readable error for the first threat found, or nullopt.
std::optional<std::string> first_threat_message(const std::string& content,
                                                const std::string& scope = "strict");

}  // namespace tools
}  // namespace dd
