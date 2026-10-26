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

// Defaults the tool layer shipped before the section existed: run_terminal handed back 8000 bytes,
// read_file 2000 lines, cut a line at 4000 bytes, and read at most 1 MiB per call. Deliberately NOT
// DEFAULT_MAX_* above — those mirror the Python module, and a config without `tool_output` must not
// silently widen the context cost of every result sixfold.
constexpr int SHIPPED_MAX_BYTES = 8000;
constexpr int SHIPPED_MAX_LINES = 2000;
constexpr int SHIPPED_MAX_LINE_LENGTH = 4000;
constexpr int SHIPPED_READ_BYTES = 1 << 20;

// The envelope for the config this process actually loaded, so a `--config` override counts rather
// than being shadowed by the home copy get_tool_output_limits reads. Keys: max_bytes, max_lines,
// max_line_length, read_bytes. Absent or unusable values fall back to the shipped numbers.
std::map<std::string, int> resolve_tool_output_from(const core::Config& cfg);

// Reset the cached limits (tests / config hot-reload).
void _reset_tool_output_limits_cache();

// Shortcuts.
int get_max_bytes();
int get_max_lines();
int get_max_line_length();

}  // namespace tools
}  // namespace dd
