#include "dd/agent/classification.hpp"
#include "dd/core/json.hpp"

#include <cctype>
#include <set>
#include <string>

namespace dd {
namespace agent {

using core::Json;

static const std::set<std::string> FILE_MUTATING_TOOL_NAMES = {"write_file", "patch"};
static const std::set<std::string> NO_EFFECT_TOOL_NAMES = {
    "read_file", "search_files", "session_search", "skill_view", "skills_list",
    "web_extract", "web_search", "vision_analyze", "browser_snapshot",
    "browser_get_images", "browser_console", "read_terminal",
};

// Python truthiness of a JSON value.
static bool json_truthy(const Json& v) {
    switch (v.type) {
        case Json::Null: return false;
        case Json::Bool: return v.b;
        case Json::Num:  return v.num != 0.0;
        case Json::Str:  return !v.str.empty();
        case Json::Arr:  return !v.arr.empty();
        case Json::Obj:  return !v.obj.empty();
    }
    return false;
}

static std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool tool_may_have_side_effect(const std::string& tool_name) {
    return !NO_EFFECT_TOOL_NAMES.count(tool_name);
}

bool file_mutation_result_landed(const std::string& tool_name, const std::string& result) {
    if (!FILE_MUTATING_TOOL_NAMES.count(tool_name)) return false;
    Json data;
    try {
        data = core::json_parse(trim(result));
    } catch (...) {
        return false;
    }
    if (data.type != Json::Obj) return false;
    const Json* err = core::json_get(data, "error");
    if (err && json_truthy(*err)) return false;
    if (tool_name == "write_file") return core::json_get(data, "bytes_written") != nullptr;
    if (tool_name == "patch") {
        const Json* s = core::json_get(data, "success");
        return s && s->type == Json::Bool && s->b;
    }
    return false;
}

}  // namespace agent
}  // namespace dd
