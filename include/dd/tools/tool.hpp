#pragma once
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "dd/core/json.hpp"

namespace dd {
namespace tools {

// One registered tool: schema + handler + availability metadata.
// Mirrors tools/registry.py::ToolEntry.
struct ToolEntry {
    std::string name;
    std::string toolset;
    dd::core::Json schema;
    std::function<std::string(const dd::core::Json&)> handler;
    std::function<bool()> check_fn;
    std::vector<std::string> requires_env;
    bool is_async = false;
    std::string description;
    std::string emoji;
    std::optional<std::size_t> max_result_size_chars;
    std::function<dd::core::Json()> dynamic_schema_overrides;
};

}  // namespace tools
}  // namespace dd
