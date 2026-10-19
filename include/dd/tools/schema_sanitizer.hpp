#pragma once
#include <string>
#include <utility>

#include "dd/core/json.hpp"

namespace dd {
namespace tools {

using core::Json;

// Port of tools/schema_sanitizer.py: sanitize tool JSON schemas for strict
// LLM backends (llama.cpp json-schema-to-grammar, Codex, Fireworks, xAI).
// Operates on the dd::core::Json tree; objects preserve insertion order.

// Deterministically map an arbitrary property key to a conforming one
// (^[a-zA-Z0-9_.-]{1,64}$, else "param").
std::string sanitize_property_key(const std::string& key);

// Sanitize an OpenAI-format tool list ([{type:function, function:{name, parameters}}]).
Json sanitize_tool_schemas(const Json& tools);

// Map sanitized property keys in model-emitted args back to wire names.
Json unrename_tool_args(const Json& params_schema, const Json& args);

// Collapse anyOf/oneOf nullable unions to the non-null branch.
Json strip_nullable_unions(const Json& schema, bool keep_nullable_hint = true);

// Strip pattern/format keywords from schema nodes; returns (tools, count).
std::pair<Json, int> strip_pattern_and_format(const Json& tools);

// Strip enum keywords whose string values contain '/'; returns (tools, count).
std::pair<Json, int> strip_slash_enum(const Json& tools);

}  // namespace tools
}  // namespace dd
