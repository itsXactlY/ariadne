#pragma once
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dd/tools/tool.hpp"

namespace dd {
namespace tools {

// Singleton-style registry collecting tool schemas + handlers.
// Faithful port of tools/registry.py::ToolRegistry.
class ToolRegistry {
public:
    // Register a tool (called at "module import" time by each tool file).
    void register_tool(const ToolEntry& entry);
    void deregister(const std::string& name);

    void register_toolset_alias(const std::string& alias, const std::string& toolset);
    std::map<std::string, std::string> get_registered_toolset_aliases() const;
    std::string get_toolset_alias_target(const std::string& alias) const;  // "" if none

    std::string get_toolset_for_tool(const std::string& name) const;  // "" if none
    std::vector<dd::core::Json> get_definitions(const std::vector<std::string>& tool_names,
                                                bool quiet = false) const;
    std::string dispatch(const std::string& name, const dd::core::Json& args) const;

    std::size_t get_max_result_size(const std::string& name,
                                    std::size_t def = 0) const;  // 0 => global default
    std::vector<std::string> get_all_tool_names() const;
    const dd::core::Json* get_schema(const std::string& name) const;  // null if absent
    std::string get_emoji(const std::string& name, const std::string& def = "\xE2\x9A\xA1") const;
    std::map<std::string, std::string> get_tool_to_toolset_map() const;

    bool is_toolset_available(const std::string& toolset) const;
    std::map<std::string, bool> check_toolset_requirements() const;
    std::vector<std::string> get_available_toolsets() const;
    std::vector<std::string> check_tool_availability_available() const;

    std::size_t generation() const { return generation_; }

private:
    std::map<std::string, ToolEntry> tools_;
    std::map<std::string, std::function<bool()>> toolset_checks_;
    std::map<std::string, std::string> toolset_aliases_;
    std::size_t generation_ = 0;
};

// JSON helpers for tool handlers (registry.py::tool_error / tool_result).
std::string tool_error(const std::string& message);

}  // namespace tools
}  // namespace dd
