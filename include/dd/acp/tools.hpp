#pragma once
// Pure subset of acp_adapter/tools.py: get_tool_kind (static TOOL_KIND_MAP
// lookup), build_tool_title (string formatting), and extract_locations (path/
// line extraction from tool args -> ToolCallLocation). NOT ported: make_tool_call_id
// (uuid RNG), build_tool_start / build_tool_complete (construct acp.schema event
// objects with no C++ analogue). ToolCallLocation.line models acp's untyped line
// as a decimal string (ints stringified to match Python str()).
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "dd/core/json.hpp"

namespace dd {
namespace acp {

using core::Json;
using core::json_get;

struct ToolCallLocation {
    std::string path;
    std::optional<std::string> line;
};

// stringify a Json value the way Python str() would for our corpora.
inline std::string json_as_str(const Json* v) {
    if (!v) return "";
    switch (v->type) {
        case Json::Str: return v->str;
        case Json::Num: {
            double d = v->num;
            if (d == static_cast<long long>(d)) return std::to_string(static_cast<long long>(d));
            return std::to_string(d);
        }
        case Json::Bool: return v->b ? "True" : "False";
        default: return "";
    }
}
// args.get(key, default) as a string; absent or non-scalar -> default.
inline std::string jget_str(const Json& args, const std::string& key,
                            const std::string& def) {
    const Json* v = json_get(args, key);
    if (!v) return def;
    if (v->type == Json::Str) return v->str;
    if (v->type == Json::Num) {
        double d = v->num;
        if (d == static_cast<long long>(d)) return std::to_string(static_cast<long long>(d));
        return std::to_string(d);
    }
    return def;
}
// truthiness of args.get(key) (the value itself, no default).
inline bool jget_truthy(const Json& args, const std::string& key) {
    const Json* v = json_get(args, key);
    if (!v) return false;
    switch (v->type) {
        case Json::Null: return false;
        case Json::Bool: return v->b;
        case Json::Num: return v->num != 0;
        case Json::Str: return !v->str.empty();
        case Json::Arr: return !v->arr.empty();
        case Json::Obj: return !v->obj.empty();
    }
    return false;
}

// --- get_tool_kind ---
inline std::string get_tool_kind(const std::string& tool_name) {
    static const std::map<std::string, std::string> kind_map = {
        {"read_file", "read"}, {"write_file", "edit"}, {"patch", "edit"},
        {"search_files", "search"}, {"terminal", "execute"}, {"process", "execute"},
        {"execute_code", "execute"}, {"web_search", "fetch"}, {"web_extract", "fetch"},
        {"browser_navigate", "fetch"}, {"browser_click", "execute"},
        {"browser_type", "execute"}, {"browser_snapshot", "read"},
        {"browser_vision", "read"}, {"browser_scroll", "execute"},
        {"browser_press", "execute"}, {"browser_back", "execute"},
        {"browser_get_images", "read"}, {"delegate_task", "execute"},
        {"vision_analyze", "read"}, {"image_generate", "execute"},
        {"text_to_speech", "execute"}, {"_thinking", "think"},
    };
    auto it = kind_map.find(tool_name);
    return it == kind_map.end() ? std::string("other") : it->second;
}

// --- build_tool_title ---
inline std::string build_tool_title(const std::string& tool_name, const Json& args) {
    if (tool_name == "terminal") {
        std::string cmd = jget_str(args, "command", "");
        if (cmd.size() > 80) cmd = cmd.substr(0, 77) + "...";
        return "terminal: " + cmd;
    }
    if (tool_name == "read_file") return "read: " + jget_str(args, "path", "?");
    if (tool_name == "write_file") return "write: " + jget_str(args, "path", "?");
    if (tool_name == "patch") {
        std::string mode = jget_str(args, "mode", "replace");
        std::string path = jget_str(args, "path", "?");
        return "patch (" + mode + "): " + path;
    }
    if (tool_name == "search_files") return "search: " + jget_str(args, "pattern", "?");
    if (tool_name == "web_search") return "web search: " + jget_str(args, "query", "?");
    if (tool_name == "web_extract") {
        const Json* urls = json_get(args, "urls");
        if (urls && urls->type == Json::Arr && !urls->arr.empty()) {
            std::string out = "extract: " + json_as_str(urls->arr[0].get());
            if (urls->arr.size() > 1)
                out += " (+" + std::to_string(urls->arr.size() - 1) + ")";
            return out;
        }
        return "web extract";
    }
    if (tool_name == "delegate_task") {
        std::string goal = jget_str(args, "goal", "");
        if (!goal.empty() && goal.size() > 60) goal = goal.substr(0, 57) + "...";
        return goal.empty() ? std::string("delegate task") : "delegate: " + goal;
    }
    if (tool_name == "execute_code") return "execute code";
    if (tool_name == "vision_analyze") {
        std::string q = jget_str(args, "question", "?");
        if (q.size() > 50) q = q.substr(0, 50);
        return "analyze image: " + q;
    }
    return tool_name;
}

// --- extract_locations ---
inline std::vector<ToolCallLocation> extract_locations(const Json& args) {
    std::vector<ToolCallLocation> out;
    if (!jget_truthy(args, "path")) return out;
    const Json* p = json_get(args, "path");
    ToolCallLocation loc;
    loc.path = (p && p->type == Json::Str) ? p->str : json_as_str(p);
    const Json* off = json_get(args, "offset");
    if (off && jget_truthy(args, "offset")) {
        loc.line = json_as_str(off);
    } else {
        const Json* ln = json_get(args, "line");
        if (ln && ln->type != Json::Null) loc.line = json_as_str(ln);
    }
    out.push_back(std::move(loc));
    return out;
}

}  // namespace acp
}  // namespace dd
