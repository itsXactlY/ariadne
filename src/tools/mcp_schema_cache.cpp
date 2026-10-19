// Pure helpers ported from tools/mcp_schema_cache.py (config_fingerprint +
// the two cache-entry accessors). The on-disk cache load/save functions are
// NOT ported here (fs/IO-coupled).
#include "dd/tools/mcp_schema_cache.hpp"
#include "dd/core/sha256.hpp"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace dd {
namespace tools {

using namespace core;

// Python truthiness for a JSON value (used by the `x or default` idiom).
static bool json_truthy(const Json& v) {
    switch (v.type) {
        case Json::Null: return false;
        case Json::Bool: return v.b;
        case Json::Num: return v.num != 0.0;
        case Json::Str: return !v.str.empty();
        case Json::Arr: return !v.arr.empty();
        case Json::Obj: return !v.obj.empty();
    }
    return false;
}

// `config.get(key) or fallback` -> deep copy of value, else fallback.
static Json or_default(const Json* v, const Json& fallback) {
    if (v && json_truthy(*v)) return json_deep_copy(*v);
    return fallback;
}

// `config.get(key)` -> value or null (no truthiness coercion).
static Json get_or_null(const Json& o, const std::string& key) {
    const Json* v = json_get(o, key);
    if (v) return json_deep_copy(*v);
    return Json{};  // Null
}

// `sorted(value or [])` over a list of strings -> Json array.
static Json sorted_str_arr(const Json* v) {
    std::vector<std::string> items;
    if (v && v->type == Json::Arr) {
        for (const auto& e : v->arr) {
            if (e && e->type == Json::Str) items.push_back(e->str);
        }
    }
    std::sort(items.begin(), items.end());
    std::vector<JsonSP> elems;
    elems.reserve(items.size());
    for (const auto& s : items) {
        Json j;
        j.type = Json::Str;
        j.str = s;
        elems.push_back(std::make_shared<Json>(std::move(j)));
    }
    return json_arr(std::move(elems));
}

std::string config_fingerprint(const Json& config) {
    const Json* tools_filter_v = json_get(config, "tools");
    Json tools_filter = or_default(tools_filter_v, json_obj());

    // payload keys inserted in alphabetical order (json.dumps sort_keys=True).
    Json payload;
    payload.type = Json::Obj;
    obj_set(payload, "args", or_default(json_get(config, "args"), json_arr()));
    obj_set(payload, "command", get_or_null(config, "command"));
    obj_set(payload, "tools_exclude", sorted_str_arr(json_get(tools_filter, "exclude")));
    obj_set(payload, "tools_include", sorted_str_arr(json_get(tools_filter, "include")));
    obj_set(payload, "transport", get_or_null(config, "transport"));
    obj_set(payload, "url", get_or_null(config, "url"));

    std::string raw = json_dump(payload);
    std::string full = sha256_hex(raw);
    return full.substr(0, 16);
}

std::vector<JsonSP> tools_from_cache_entry(const Json& entry) {
    const Json* t = json_get(entry, "tools");
    if (t && t->type == Json::Arr) return t->arr;
    return {};
}

std::vector<JsonSP> utility_tools_from_cache_entry(const Json& entry) {
    const Json* u = json_get(entry, "utility_tools");
    if (u && u->type == Json::Arr) return u->arr;
    return {};
}

}  // namespace tools
}  // namespace dd
