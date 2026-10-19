#pragma once
#include <map>
#include <string>

#include "dd/core/config.hpp"

namespace dd {
namespace tools {

// tools/tool_output_limits.py -> dd::tools
// Configurable tool-output truncation limits, read from the `tool_output`
// section of config.yaml. Defensive: any error falls back to defaults.

constexpr int DEFAULT_MAX_BYTES = 50000;
constexpr int DEFAULT_MAX_LINES = 2000;
constexpr int DEFAULT_MAX_LINE_LENGTH = 2000;

// Return value as a positive int, or `default` on any issue.
int _coerce_positive_int(const core::Json* value, int def);

// Resolved tool-output limits (keys: max_bytes, max_lines, max_line_length).
// Reads `tool_output` from config; NEVER raises. Cached for process lifetime.
const std::map<std::string, int>& get_tool_output_limits();

// Reset the cached limits (tests / config hot-reload).
void _reset_tool_output_limits_cache();

// Shortcuts.
int get_max_bytes();
int get_max_lines();
int get_max_line_length();

}  // namespace tools
}  // namespace dd
