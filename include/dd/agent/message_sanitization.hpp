#pragma once
// Pure subset of agent/message_sanitization.py: deterministic string
// sanitizers + the interrupted-tail closer. Logging, surrogate (UTF-codec)
// handling and the JSON-repair cascade (which leans on json.loads strict=False
// semantics) are intentionally NOT ported here.
#include <string>
#include <vector>
#include "dd/core/json.hpp"

namespace dd {
namespace agent {

// _escape_invalid_chars_in_json_strings: escape literal control chars (0x00-0x1F)
// that appear inside JSON string values as \uXXXX (lowercase, 4 hex digits).
std::string escape_invalid_chars_in_json_strings(const std::string& raw);

// _strip_non_ascii: keep only bytes < 0x80 (drops all non-ASCII code points).
std::string strip_non_ascii(const std::string& text);

// close_interrupted_tool_sequence: append a synthetic assistant turn when the
// transcript tail is a raw tool result. Mutates `messages` in place.
bool close_interrupted_tool_sequence(std::vector<core::Json>& messages,
                                     const std::string& final_response);

}  // namespace agent
}  // namespace dd
