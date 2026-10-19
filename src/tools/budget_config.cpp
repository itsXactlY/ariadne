// tools/budget_config.py -> dd::tools
#include "dd/tools/budget_config.hpp"

#include <limits>
#include <map>
#include <string>

namespace dd {
namespace tools {

const std::map<std::string, double> PINNED_THRESHOLDS = {
    {"read_file", std::numeric_limits<double>::infinity()},
};

BudgetConfig DEFAULT_BUDGET;

double BudgetConfig::resolve_threshold(const std::string& tool_name,
                                       const ToolRegistry& registry) const {
    auto pinned = PINNED_THRESHOLDS.find(tool_name);
    if (pinned != PINNED_THRESHOLDS.end()) return pinned->second;
    auto ov = tool_overrides.find(tool_name);
    if (ov != tool_overrides.end()) return static_cast<double>(ov->second);
    return static_cast<double>(
        registry.get_max_result_size(tool_name,
                                     static_cast<std::size_t>(default_result_size)));
}

}  // namespace tools
}  // namespace dd
