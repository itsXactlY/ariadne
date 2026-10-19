#pragma once
#include <string>
#include "dd/core/json.hpp"

namespace dd {
namespace agent {

// Faithful port of agent/tool_result_classification.py.
bool tool_may_have_side_effect(const std::string& tool_name);
// Python signature takes result: Any; tool-result payloads are JSON strings at the call site,
// so the C++ port takes the string payload directly.
bool file_mutation_result_landed(const std::string& tool_name, const std::string& result);

}  // namespace agent
}  // namespace dd
