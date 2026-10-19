#pragma once
#include <limits>
#include <map>
#include <string>

#include "dd/tools/registry.hpp"

namespace dd {
namespace tools {

// tools/budget_config.py -> dd::tools
// Configurable budget constants for tool-result persistence.
// Per-tool resolution priority: pinned > tool_overrides > registry per-tool > default.

// PINNED_THRESHOLDS: read_file never persists (infinity).
extern const std::map<std::string, double> PINNED_THRESHOLDS;

constexpr int DEFAULT_RESULT_SIZE_CHARS = 100000;
constexpr int DEFAULT_TURN_BUDGET_CHARS = 200000;
constexpr int DEFAULT_PREVIEW_SIZE_CHARS = 1500;

// Immutable budget constants for the 3-layer tool-result persistence system.
// Layer 2 (per-result): resolve_threshold(tool_name) -> threshold in chars.
// Layer 3 (per-turn):   turn_budget -> aggregate char budget across all tool
//                       results in a single assistant turn.
// Preview:              preview_size -> inline snippet size after persistence.
struct BudgetConfig {
    int default_result_size = DEFAULT_RESULT_SIZE_CHARS;
    int turn_budget = DEFAULT_TURN_BUDGET_CHARS;
    int preview_size = DEFAULT_PREVIEW_SIZE_CHARS;
    std::map<std::string, int> tool_overrides;

    // Resolve the persistence threshold for a tool. Returns infinity (double)
    // for pinned tools that must never persist.
    double resolve_threshold(const std::string& tool_name,
                             const ToolRegistry& registry) const;
};

// DEFAULT_BUDGET = BudgetConfig()
extern BudgetConfig DEFAULT_BUDGET;

}  // namespace tools
}  // namespace dd
