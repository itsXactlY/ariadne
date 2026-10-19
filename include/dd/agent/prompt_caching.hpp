#pragma once
// Full pure port of agent/prompt_caching.py (Anthropic system_and_3 caching).
// API matches the existing consumer tests/agent_misc_test.cpp: a messages list
// is a std::vector<Json> (each element a message object). No time/fs coupling.
#include <string>
#include <vector>
#include "dd/core/json.hpp"

namespace dd {
namespace agent {

// Deep copy + place up to 4 cache_control breakpoints (system prompt + last 3
// non-system messages). Returns the updated messages vector.
std::vector<core::Json> apply_anthropic_cache_control(
    const std::vector<core::Json>& api_messages,
    const std::string& cache_ttl,
    bool native_anthropic);

}  // namespace agent
}  // namespace dd
