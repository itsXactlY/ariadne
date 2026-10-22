#include "dd/tools/registry.hpp"
#include "dd/core/json.hpp"

#include <algorithm>
#include <set>
#include <iostream>

namespace dd {
namespace tools {

static constexpr std::size_t DEFAULT_RESULT_SIZE_CHARS = 100000;

void ToolRegistry::register_tool(const ToolEntry& entry) {
    auto it = tools_.find(entry.name);
    if (it != tools_.end() && it->second.toolset != entry.toolset) {
        // Tool name collision across toolsets: warn, then overwrite.
        std::cerr << "[warn] Tool name collision: '" << entry.name << "' (toolset '"
                  << it->second.toolset << "') overwritten by '" << entry.toolset << "'\n";
    }
    tools_[entry.name] = entry;
    if (entry.check_fn && !toolset_checks_.count(entry.toolset))
        toolset_checks_[entry.toolset] = entry.check_fn;
    // An MCP server has one entry per tool, all under the server's name as toolset.
    // That name also becomes an alias for itself, so the requirement check and the
    // availability map see the server even after every one of its tools is gone:
    // a dead server is then a visible error, not silently invisible.
    if (!entry.toolset.empty() && !toolset_aliases_.count(entry.toolset))
        toolset_aliases_[entry.toolset] = entry.toolset;
    ++generation_;
}

void ToolRegistry::deregister(const std::string& name) {
    auto it = tools_.find(name);
    if (it == tools_.end()) return;
    std::string ts = it->second.toolset;
    tools_.erase(it);
    if (toolset_checks_.count(ts)) {
        bool any = false;
        for (const auto& [n, e] : tools_)
            if (e.toolset == ts) { any = true; break; }
        if (!any) toolset_checks_.erase(ts);
    }
    ++generation_;
}

void ToolRegistry::register_toolset_alias(const std::string& alias, const std::string& toolset) {
    auto it = toolset_aliases_.find(alias);
    if (it != toolset_aliases_.end() && it->second != toolset) {
        std::cerr << "[warn] Toolset alias collision: '" << alias << "' ("
                  << it->second << ") overwritten by " << toolset << "\n";
    }
    toolset_aliases_[alias] = toolset;
    ++generation_;
}

std::map<std::string, std::string> ToolRegistry::get_registered_toolset_aliases() const {
    return toolset_aliases_;
}

std::string ToolRegistry::get_toolset_alias_target(const std::string& alias) const {
    auto it = toolset_aliases_.find(alias);
    return it == toolset_aliases_.end() ? std::string() : it->second;
}

std::string ToolRegistry::get_toolset_for_tool(const std::string& name) const {
    auto it = tools_.find(name);
    if (it != tools_.end()) return it->second.toolset;
    // Only the explicit aliases resolve a prefix: wire names carry their own
    // separators (`mazemaker_recall` is a mazemaker tool, not a `maze` one), so
    // guessing from a leading underscore turns every snake_case MCP name into
    // a phantom member of a shorter toolset.
    if (const auto a = toolset_aliases_.find(name); a != toolset_aliases_.end()) return a->second;
    return {};
}

bool ToolRegistry::is_toolset_available(const std::string& toolset) const {
    auto it = toolset_checks_.find(toolset);
    if (it == toolset_checks_.end()) return true;
    try {
        return static_cast<bool>(it->second());
    } catch (...) {
        return false;
    }
}

std::map<std::string, bool> ToolRegistry::check_toolset_requirements() const {
    std::set<std::string> toolsets;
    for (const auto& [n, e] : tools_) toolsets.insert(e.toolset);
    std::map<std::string, bool> out;
    for (const auto& ts : toolsets) out[ts] = is_toolset_available(ts);
    return out;
}

std::vector<std::string> ToolRegistry::get_available_toolsets() const {
    std::set<std::string> toolsets;
    for (const auto& [n, e] : tools_) toolsets.insert(e.toolset);
    std::vector<std::string> out;
    for (const auto& ts : toolsets)
        if (is_toolset_available(ts)) out.push_back(ts);
    return out;
}

std::vector<std::string> ToolRegistry::check_tool_availability_available() const {
    return get_available_toolsets();
}

std::size_t ToolRegistry::get_max_result_size(const std::string& name, std::size_t def) const {
    auto it = tools_.find(name);
    if (it != tools_.end() && it->second.max_result_size_chars)
        return *it->second.max_result_size_chars;
    if (def != 0) return def;
    return DEFAULT_RESULT_SIZE_CHARS;
}

std::vector<std::string> ToolRegistry::get_all_tool_names() const {
    std::vector<std::string> out;
    out.reserve(tools_.size());
    for (const auto& [n, e] : tools_) out.push_back(n);
    return out;  // std::map is already sorted
}

const dd::core::Json* ToolRegistry::get_schema(const std::string& name) const {
    auto it = tools_.find(name);
    return it == tools_.end() ? nullptr : &it->second.schema;
}

std::string ToolRegistry::get_emoji(const std::string& name, const std::string& def) const {
    auto it = tools_.find(name);
    if (it != tools_.end() && !it->second.emoji.empty()) return it->second.emoji;
    return def;
}

std::map<std::string, std::string> ToolRegistry::get_tool_to_toolset_map() const {
    std::map<std::string, std::string> out;
    for (const auto& [n, e] : tools_) out[n] = e.toolset;
    return out;
}

std::vector<dd::core::Json> ToolRegistry::get_definitions(const std::vector<std::string>& tool_names,
                                                          bool quiet) const {
    std::vector<dd::core::Json> result;
    std::vector<std::string> sorted(tool_names);
    std::sort(sorted.begin(), sorted.end());
    for (const auto& name : sorted) {
        auto it = tools_.find(name);
        if (it == tools_.end()) continue;
        const ToolEntry& e = it->second;
        if (e.check_fn) {
            bool ok = true;
            try { ok = static_cast<bool>(e.check_fn()); } catch (...) { ok = false; }
            if (!ok) {
                if (!quiet) std::cerr << "[debug] Tool " << name << " unavailable (check failed)\n";
                continue;
            }
        }
        // schema_with_name = {**schema, "name": name}
        dd::core::Json fn = e.schema;
        fn.type = dd::core::Json::Obj;
        bool has_name = false;
        for (auto& [k, sp] : fn.obj)
            if (k == "name") { sp = std::make_shared<dd::core::Json>(dd::core::json_str(name)); has_name = true; break; }
        if (!has_name) fn.obj.emplace_back("name", std::make_shared<dd::core::Json>(dd::core::json_str(name)));
        // dynamic overrides
        if (e.dynamic_schema_overrides) {
            try {
                dd::core::Json ov = e.dynamic_schema_overrides();
                if (ov.type == dd::core::Json::Obj)
                    for (auto& [k, sp] : ov.obj) {
                        bool found = false;
                        for (auto& [fk, fsp] : fn.obj)
                            if (fk == k) { fsp = sp; found = true; break; }
                        if (!found) fn.obj.emplace_back(k, sp);
                    }
            } catch (...) { /* keep static schema */ }
        }
        dd::core::Json wrapper = dd::core::json_obj(
            {{"type", std::make_shared<dd::core::Json>(dd::core::json_str("function"))},
             {"function", std::make_shared<dd::core::Json>(std::move(fn))}});
        result.push_back(std::move(wrapper));
    }
    return result;
}

std::string ToolRegistry::dispatch(const std::string& name, const dd::core::Json& args) const {
    auto it = tools_.find(name);
    if (it == tools_.end())
        return dd::core::json_dump(dd::core::json_obj(
            {{"error", std::make_shared<dd::core::Json>(dd::core::json_str("Unknown tool: " + name))}}));
    const ToolEntry& e = it->second;
    try {
        if (!e.handler)
            return dd::core::json_dump(dd::core::json_obj(
                {{"error", std::make_shared<dd::core::Json>(dd::core::json_str("Tool execution failed: no handler"))}}));
        return e.handler(args);
    } catch (const std::exception& ex) {
        return dd::core::json_dump(dd::core::json_obj(
            {{"error", std::make_shared<dd::core::Json>(dd::core::json_str(
                 std::string("Tool execution failed: ") + ex.what()))}}));
    } catch (...) {
        return dd::core::json_dump(dd::core::json_obj(
            {{"error", std::make_shared<dd::core::Json>(dd::core::json_str(
                 std::string("Tool execution failed: unknown")) )}}));
    }
}

std::string tool_error(const std::string& message) {
    return dd::core::json_dump(dd::core::json_obj(
        {{"error", std::make_shared<dd::core::Json>(dd::core::json_str(message))}}));
}

}  // namespace tools
}  // namespace dd
