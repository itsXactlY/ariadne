// Port of tools/schema_sanitizer.py — recursive JSON-Schema sanitizer.
#include "dd/tools/schema_sanitizer.hpp"

#include <map>
#include <set>
#include <string>
#include <vector>

#include "dd/core/json.hpp"

namespace dd {
namespace tools {

using namespace core;
using Json::Obj; using Json::Arr; using Json::Str; using Json::Bool;

// ---- key-pattern helpers -------------------------------------------------
static bool is_good_prop_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
}

// ^[a-zA-Z0-9_.-]{1,64}$ (any non-ASCII codepoint fails, so byte scan is fine)
static bool prop_key_ok(const std::string& s) {
    if (s.size() < 1 || s.size() > 64) return false;
    for (char c : s)
        if (!is_good_prop_char(c)) return false;
    return true;
}

// ---- mutable object helpers ---------------------------------------------
static Json* mget(Json& o, const std::string& key) {
    for (auto& kv : o.obj)
        if (kv.first == key) return kv.second.get();
    return nullptr;
}

static void obj_erase(Json& o, const std::string& key) {
    for (auto it = o.obj.begin(); it != o.obj.end(); ++it) {
        if (it->first == key) {
            o.obj.erase(it);
            return;
        }
    }
}

static bool has_key(const Json& o, const std::string& key) {
    return json_get(o, key) != nullptr;
}

// ---- sanitize_property_key ----------------------------------------------
std::string sanitize_property_key(const std::string& key) {
    // Python: _PROP_KEY_BAD_CHARS.sub("_", key)[:64] — the regex and the
    // slice operate on Unicode codepoints, so each non-conforming codepoint
    // (including every non-ASCII one) collapses to a single '_'.
    std::string out;
    out.reserve(key.size());
    size_t i = 0, cps = 0;
    while (i < key.size() && cps < 64) {
        unsigned char c = (unsigned char)key[i];
        if (c < 0x80) {
            out.push_back(is_good_prop_char((char)c) ? (char)c : '_');
            ++i;
        } else {
            size_t len = 1;
            if ((c & 0xE0) == 0xC0) len = 2;
            else if ((c & 0xF0) == 0xE0) len = 3;
            else if ((c & 0xF8) == 0xF0) len = 4;
            if (i + len > key.size()) len = 1;  // malformed sequence
            out.push_back('_');
            i += len;
        }
        ++cps;
    }
    if (out.empty()) return "param";
    return out;
}

// ---- _rename_property_keys ----------------------------------------------
static std::map<std::string, std::string> rename_property_keys(
    const Json& props, const std::string& path) {
    (void)path;  // used only for debug logging in the Python source
    std::map<std::string, std::string> renames;
    std::set<std::string> taken;
    for (auto& kv : props.obj)
        if (prop_key_ok(kv.first)) taken.insert(kv.first);
    for (auto& kv : props.obj) {
        const std::string& key = kv.first;
        if (prop_key_ok(key)) continue;
        std::string base = sanitize_property_key(key);
        std::string candidate = base;
        int i = 2;
        while (taken.count(candidate)) {
            std::string suffix = "_" + std::to_string(i);
            int cut = 64 - (int)suffix.size();
            if (cut < 0) cut = 0;
            std::string b = (cut < (int)base.size()) ? base.substr(0, cut) : base;
            candidate = b + suffix;
            ++i;
        }
        taken.insert(candidate);
        renames[key] = candidate;
    }
    return renames;
}

// ---- unrename_tool_args --------------------------------------------------
Json unrename_tool_args(const Json& params_schema, const Json& args) {
    if (params_schema.type != Obj || args.type != Obj) return args;
    const Json* props = json_get(params_schema, "properties");
    if (!props || props->type != Obj) return args;
    auto renames = rename_property_keys(*props, "<unrename>");
    std::map<std::string, std::string> reverse;
    for (auto& kv : renames) reverse[kv.second] = kv.first;

    Json out = json_obj();
    for (auto& kv : args.obj) {
        const std::string& key = kv.first;
        const Json& value = *kv.second;
        std::string orig = reverse.count(key) ? reverse[key] : key;
        const Json* subschema = json_get(*props, orig);
        Json newval = value;
        if (subschema && subschema->type == Obj) {
            if (value.type == Obj) {
                newval = unrename_tool_args(*subschema, value);
            } else if (value.type == Arr) {
                const Json* items = json_get(*subschema, "items");
                if (items && items->type == Obj) {
                    Json na = json_arr();
                    for (auto& el : value.arr) {
                        if (el->type == Obj)
                            arr_push(na, unrename_tool_args(*items, *el));
                        else
                            arr_push(na, *el);
                    }
                    newval = na;
                }
            }
        }
        obj_set(out, orig, newval);
    }
    return out;
}

// ---- _strip_ref_siblings -------------------------------------------------
static Json strip_ref_siblings(const Json& node) {
    if (node.type == Arr) {
        Json out = json_arr();
        for (auto& el : node.arr) arr_push(out, strip_ref_siblings(*el));
        return out;
    }
    if (node.type != Obj) return node;
    Json out = json_obj();
    for (auto& kv : node.obj) obj_set(out, kv.first, strip_ref_siblings(*kv.second));
    if (has_key(out, "$ref")) obj_erase(out, "default");
    return out;
}

// ---- _strip_top_level_combinators ---------------------------------------
static Json strip_top_level_combinators(const Json& params) {
    if (params.type != Obj) return params;
    Json out = params;  // shallow copy: shares children, copies member vector
    for (const char* k : {"allOf", "anyOf", "oneOf", "enum", "not"})
        obj_erase(out, k);
    return out;
}

// ---- strip_nullable_unions ----------------------------------------------
Json strip_nullable_unions(const Json& schema, bool keep_nullable_hint) {
    if (schema.type == Arr) {
        Json out = json_arr();
        for (auto& el : schema.arr)
            arr_push(out, strip_nullable_unions(*el, keep_nullable_hint));
        return out;
    }
    if (schema.type != Obj) return schema;

    Json stripped = json_obj();
    for (auto& kv : schema.obj)
        obj_set(stripped, kv.first, strip_nullable_unions(*kv.second, keep_nullable_hint));

    for (const char* key : {"anyOf", "oneOf"}) {
        const Json* variants = json_get(stripped, key);
        if (!variants || variants->type != Arr) continue;
        std::vector<JsonSP> non_null;
        for (auto& el : variants->arr) {
            bool is_null = false;
            if (el->type == Obj) {
                const Json* t = json_get(*el, "type");
                is_null = (t && t->type == Str && t->str == "null");
            }
            if (!is_null) non_null.push_back(el);
        }
        if (non_null.size() == 1 && non_null.size() != variants->arr.size()) {
            Json replacement =
                (non_null[0]->type == Obj) ? Json(*non_null[0]) : json_obj();
            if (keep_nullable_hint && !has_key(replacement, "nullable"))
                obj_set(replacement, "nullable", json_bool(true));
            for (const char* meta : {"title", "description", "default", "examples"}) {
                if (has_key(stripped, meta) && !has_key(replacement, meta)) {
                    if (std::string(meta) == "default" && has_key(replacement, "$ref"))
                        continue;
                    obj_set(replacement, meta, *json_get(stripped, meta));
                }
            }
            return strip_nullable_unions(replacement, keep_nullable_hint);
        }
    }
    return stripped;
}

// ---- _sanitize_node ------------------------------------------------------
static Json sanitize_node(const Json& node, const std::string& path) {
    if (node.type == Str) {
        static const std::set<std::string> bare = {
            "object", "string", "number", "integer", "boolean", "array", "null"};
        Json o = json_obj();
        if (bare.count(node.str)) {
            if (node.str != "object") {
                obj_set(o, "type", json_str(node.str));
            } else {
                obj_set(o, "type", json_str("object"));
                obj_set(o, "properties", json_obj());
            }
            return o;
        }
        obj_set(o, "type", json_str("object"));
        obj_set(o, "properties", json_obj());
        return o;
    }
    if (node.type == Arr) {
        Json out = json_arr();
        size_t i = 0;
        for (auto& el : node.arr) {
            arr_push(out, sanitize_node(*el, path + "[" + std::to_string(i) + "]"));
            ++i;
        }
        return out;
    }
    if (node.type != Obj) return node;

    std::map<std::string, std::string> prop_renames;
    const Json* props = json_get(node, "properties");
    if (props && props->type == Obj)
        prop_renames = rename_property_keys(*props, path + ".properties");

    Json out = json_obj();
    for (auto& kv : node.obj) {
        const std::string& key = kv.first;
        const Json& value = *kv.second;

        if (key == "type" && value.type == Arr) {
            bool has_null = false;
            std::vector<std::string> non_null;
            for (auto& el : value.arr) {
                if (el->type == Str) {
                    if (el->str == "null") has_null = true;
                    else non_null.push_back(el->str);
                }
            }
            if (non_null.size() == 1) {
                obj_set(out, "type", json_str(non_null[0]));
                if (has_null && !has_key(out, "nullable"))
                    obj_set(out, "nullable", json_bool(true));
                continue;
            }
            if (non_null.size() >= 2) {
                Json anyof = json_arr();
                for (auto& t : non_null) {
                    Json o = json_obj();
                    obj_set(o, "type", json_str(t));
                    arr_push(anyof, o);
                }
                obj_set(out, "anyOf", anyof);
                if (has_null && !has_key(out, "nullable"))
                    obj_set(out, "nullable", json_bool(true));
                continue;
            }
            obj_set(out, "type", json_str(has_null ? "null" : "object"));
            continue;
        }

        if ((key == "properties" || key == "$defs" || key == "definitions") &&
            value.type == Obj) {
            std::map<std::string, std::string> renames =
                (key == "properties") ? prop_renames
                                       : std::map<std::string, std::string>{};
            Json new_props = json_obj();
            for (auto& skv : value.obj) {
                std::string out_k =
                    renames.count(skv.first) ? renames[skv.first] : skv.first;
                obj_set(new_props, out_k,
                        sanitize_node(*skv.second,
                                      path + "." + key + "." + out_k));
            }
            obj_set(out, key, new_props);
        } else if (key == "items" || key == "additionalProperties") {
            if (value.type == Bool)
                obj_set(out, key, value);
            else
                obj_set(out, key, sanitize_node(value, path + "." + key));
        } else if ((key == "anyOf" || key == "oneOf" || key == "allOf") &&
                   value.type == Arr) {
            Json na = json_arr();
            size_t i = 0;
            for (auto& el : value.arr) {
                arr_push(na, sanitize_node(*el,
                                           path + "." + key + "[" + std::to_string(i) + "]"));
                ++i;
            }
            obj_set(out, key, na);
        } else if (key == "required" || key == "enum" || key == "examples" ||
                   key == "dependentRequired") {
            if (key == "required" && !prop_renames.empty() && value.type == Arr) {
                Json na = json_arr();
                for (auto& el : value.arr) {
                    if (el->type == Str) {
                        std::string r = el->str;
                        arr_push(na, json_str(prop_renames.count(r) ? prop_renames[r] : r));
                    } else {
                        arr_push(na, *el);
                    }
                }
                obj_set(out, key, na);
            } else if (value.type == Arr || value.type == Obj) {
                obj_set(out, key, json_deep_copy(value));
            } else {
                obj_set(out, key, value);
            }
        } else {
            if (value.type == Obj || value.type == Arr)
                obj_set(out, key, sanitize_node(value, path + "." + key));
            else
                obj_set(out, key, value);
        }
    }

    // object-typed node missing properties
    const Json* t = json_get(out, "type");
    if (t && t->type == Str && t->str == "object") {
        const Json* p = json_get(out, "properties");
        if (!p || p->type != Obj) obj_set(out, "properties", json_obj());
    }
    // required must reference existing properties
    t = json_get(out, "type");
    const Json* req = json_get(out, "required");
    if (t && t->type == Str && t->str == "object" && req && req->type == Arr) {
        const Json* props2 = json_get(out, "properties");
        std::vector<std::string> valid;
        for (auto& el : req->arr) {
            if (el->type == Str && props2 && props2->type == Obj &&
                json_get(*props2, el->str))
                valid.push_back(el->str);
        }
        if (valid.empty()) {
            obj_erase(out, "required");
        } else if (valid.size() != req->arr.size()) {
            Json na = json_arr();
            for (auto& r : valid) arr_push(na, json_str(r));
            obj_set(out, "required", na);
        }
    }
    return out;
}

// ---- _sanitize_single_tool ----------------------------------------------
static Json sanitize_single_tool(const Json& tool) {
    Json out = json_deep_copy(tool);
    Json* fn = mget(out, "function");
    if (!fn || fn->type != Obj) return out;

    Json* params = mget(*fn, "parameters");
    if (!params || params->type != Obj) {
        Json np = json_obj();
        obj_set(np, "type", json_str("object"));
        obj_set(np, "properties", json_obj());
        obj_set(*fn, "parameters", np);
        return out;
    }

    Json* namej = mget(*fn, "name");
    std::string name = (namej && namej->type == Str) ? namej->str : "<tool>";

    obj_set(*fn, "parameters", sanitize_node(*params, name));

    Json* top = mget(*fn, "parameters");
    if (!top || top->type != Obj) {
        Json np = json_obj();
        obj_set(np, "type", json_str("object"));
        obj_set(np, "properties", json_obj());
        obj_set(*fn, "parameters", np);
    } else {
        Json* tt = mget(*top, "type");
        if (!tt || tt->type != Str || tt->str != "object")
            obj_set(*top, "type", json_str("object"));
        Json* pp = mget(*top, "properties");
        if (!pp || pp->type != Obj) obj_set(*top, "properties", json_obj());
    }

    obj_set(*fn, "parameters", strip_nullable_unions(*mget(*fn, "parameters"), true));
    obj_set(*fn, "parameters",
            strip_top_level_combinators(*mget(*fn, "parameters")));
    obj_set(*fn, "parameters", strip_ref_siblings(*mget(*fn, "parameters")));
    return out;
}

// ---- sanitize_tool_schemas ----------------------------------------------
Json sanitize_tool_schemas(const Json& tools) {
    if (tools.type != Arr || tools.arr.empty()) return tools;
    Json out = json_arr();
    for (auto& el : tools.arr) arr_push(out, sanitize_single_tool(*el));
    return out;
}

// ---- strip_pattern_and_format -------------------------------------------
static void walk_strip_pattern(Json& node, int& stripped) {
    if (node.type == Obj) {
        bool is_schema = has_key(node, "type") || has_key(node, "anyOf") ||
                         has_key(node, "oneOf") || has_key(node, "allOf");
        std::vector<std::string> keys;
        keys.reserve(node.obj.size());
        for (auto& kv : node.obj) keys.push_back(kv.first);
        for (auto& key : keys) {
            if (is_schema && (key == "pattern" || key == "format")) {
                obj_erase(node, key);
                ++stripped;
                continue;
            }
            Json* child = mget(node, key);
            if (child) walk_strip_pattern(*child, stripped);
        }
    } else if (node.type == Arr) {
        for (auto& el : node.arr) walk_strip_pattern(*el, stripped);
    }
}

std::pair<Json, int> strip_pattern_and_format(const Json& tools) {
    if (tools.type != Arr || tools.arr.empty()) return {tools, 0};
    Json out = json_deep_copy(tools);
    int stripped = 0;
    for (auto& el : out.arr) {
        if (el->type != Obj) continue;
        Json* fn = mget(*el, "function");
        if (fn && fn->type == Obj) {
            Json* params = mget(*fn, "parameters");
            if (params && params->type == Obj) {
                walk_strip_pattern(*params, stripped);
                continue;
            }
        }
        Json* params = mget(*el, "parameters");
        if (params && params->type == Obj) walk_strip_pattern(*params, stripped);
    }
    return {out, stripped};
}

// ---- strip_slash_enum ---------------------------------------------------
static void walk_strip_enum(Json& node, int& stripped) {
    if (node.type == Obj) {
        const Json* en = json_get(node, "enum");
        if (en && en->type == Arr) {
            bool has_slash = false;
            for (auto& el : en->arr) {
                if (el->type == Str && el->str.find('/') != std::string::npos) {
                    has_slash = true;
                    break;
                }
            }
            if (has_slash) {
                obj_erase(node, "enum");
                ++stripped;
            }
        }
        std::vector<std::string> keys;
        keys.reserve(node.obj.size());
        for (auto& kv : node.obj) keys.push_back(kv.first);
        for (auto& key : keys) {
            Json* child = mget(node, key);
            if (child) walk_strip_enum(*child, stripped);
        }
    } else if (node.type == Arr) {
        for (auto& el : node.arr) walk_strip_enum(*el, stripped);
    }
}

std::pair<Json, int> strip_slash_enum(const Json& tools) {
    if (tools.type != Arr || tools.arr.empty()) return {tools, 0};
    Json out = json_deep_copy(tools);
    int stripped = 0;
    for (auto& el : out.arr) {
        if (el->type != Obj) continue;
        Json* fn = mget(*el, "function");
        if (fn && fn->type == Obj) {
            Json* params = mget(*fn, "parameters");
            if (params && params->type == Obj) {
                walk_strip_enum(*params, stripped);
                continue;
            }
        }
        Json* params = mget(*el, "parameters");
        if (params && params->type == Obj) walk_strip_enum(*params, stripped);
    }
    return {out, stripped};
}

}  // namespace tools
}  // namespace dd
