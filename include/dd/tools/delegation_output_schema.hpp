#pragma once
// Pure helpers ported from tools/delegation_output_schema.py.
// NOT ported (not claimed here): validate_output (needs the jsonschema library).
// coerce_output_schema runs the structural checks (None / str-parse / dict) but
// SKIPS the jsonschema meta-validation step, taking the same code path the
// Python module takes when jsonschema is unavailable (ImportError -> skip).
#include "dd/core/json.hpp"
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dd {
namespace tools {

using core::Json;

extern const std::string CONTRACT_HEADER;  // "OUTPUT CONTRACT (machine-validated)"
extern const int MAX_SCHEMA_RETRIES;       // 1

// (schema, None) when usable; (None, error) when not. None input -> (None, None).
std::pair<std::optional<Json>, std::optional<std::string>> coerce_output_schema(const Json& raw);

// Append the OUTPUT CONTRACT block to a child's context (empty context -> block alone).
std::string append_output_contract(const std::string& context, const Json& schema);

// Best-effort extraction of a JSON payload from model output.
std::string extract_json_candidate(const std::string& text);

// Build the single bounded retry turn carrying the validation errors verbatim.
std::string build_retry_message(const std::vector<std::string>& errors);

}  // namespace tools
}  // namespace dd
