// constants.hpp — daedalus_constants.py pure reasoning/config/WSL/module cluster.
// Ported (pure, deterministic):
//   * parse_reasoning_effort (934) — Json value -> {"enabled":bool[,effort]} or null
//   * _canonical_model_variants (961) — bounded spelling-variant generator
//   * resolve_per_model_reasoning_effort (1042) — tolerant override lookup
//   * resolve_reasoning_config (1077) — chokepoint: per-model then global effort
//   * windows_path_to_wsl (1166) / wsl_unc_path_to_posix (1178) — path translators
//   * is_first_party_module (1393) — exact-root membership
// NOT ported (coupled):
//   * translate_cwd_for_wsl_backend (1191) — calls env-coupled is_wsl()
//   * is_wsl/is_termux/is_container/... env+fs probes, _container_detected cache
//   * platform/env constants and the YAML loaders in this module
//   * partial_update_hint (1406) — exception-type coupled (exc.name)
// Boundary: model-config values whose model_cfg["default"/"model"] are non-strings
//   (Num/Bool) fall back to "" here; config model names are strings in practice.
// Note: py str() of an Arr/Obj/Num reasoning_effort yields no valid level -> None,
//   reproduced as json_null() (a container/number never matches an effort word).
#ifndef DD_CORE_CONSTANTS_HPP
#define DD_CORE_CONSTANTS_HPP
#include "dd/core/json.hpp"
#include <set>
#include <vector>
#include <optional>
#include <string>
#include <cctype>
#include <regex>

namespace dd {
namespace core {

namespace {

inline std::string to_lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

inline std::string trim_ws(const std::string& s) {
    auto isws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
    size_t a = 0, b = s.size();
    while (a < b && isws(s[a])) ++a;
    while (b > a && isws(s[b - 1])) --b;
    return s.substr(a, b - a);
}

inline std::string replace_char(const std::string& s, char from, char to) {
    std::string out = s;
    for (char& c : out) if (c == from) c = to;
    return out;
}

inline std::string dash_to_dot(const std::string& s) {
    static thread_local std::regex re(R"((\d)-(\d))");
    return std::regex_replace(s, re, "$1.$2");
}

inline std::string dot_to_dash(const std::string& s) {
    static thread_local std::regex re(R"((\d)\.(\d))");
    return std::regex_replace(s, re, "$1-$2");
}

inline const std::set<std::string>& VALID_REASONING_EFFORTS() {
    static const std::set<std::string> v{"minimal", "low", "medium", "high", "xhigh", "max", "ultra"};
    return v;
}

inline const std::vector<std::string>& KNOWN_PROVIDERS() {
    static const std::vector<std::string> v{
        "anthropic", "openai", "google", "openrouter", "groq", "mistral",
        "xai", "cohere", "perplexity", "together", "fireworks", "deepseek"};
    return v;
}

inline const std::vector<std::string>& KNOWN_AGGREGATORS() {
    static const std::vector<std::string> v{"openrouter", "opencode", "fireworks", "groq", "together"};
    return v;
}

inline const std::set<std::string>& FIRST_PARTY_MODULE_ROOTS() {
    static const std::set<std::string> v{
        "agent", "acp_adapter", "cli", "cron", "gateway", "model_tools",
        "plugins", "providers", "tools", "toolsets", "run_agent", "tui_gateway", "utils"};
    return v;
}

}  // anon

// ---- parse_reasoning_effort -------------------------------------------------
// Json effort -> Json result; json_null() == Python None.
inline core::Json parse_reasoning_effort(const Json& effort) {
    if (effort.type == Json::Bool) {
        if (!effort.b) {
            Json o = json_obj();
            obj_set(o, "enabled", json_bool(false));
            return o;
        }
        return json_null();  // effort is True -> None
    }
    if (effort.type != Json::Str) return json_null();  // None / number / container -> None
    std::string e = trim_ws(effort.str);
    if (e.empty()) return json_null();
    e = to_lower(trim_ws(effort.str));
    if (e == "none" || e == "false" || e == "disabled") {
        Json o = json_obj();
        obj_set(o, "enabled", json_bool(false));
        return o;
    }
    if (VALID_REASONING_EFFORTS().count(e)) {
        Json o = json_obj();
        obj_set(o, "enabled", json_bool(true));
        obj_set(o, "effort", json_str(e));
        return o;
    }
    return json_null();
}

// ---- _canonical_model_variants ----------------------------------------------
inline std::vector<std::string> _canonical_model_variants(const std::string& model) {
    std::vector<std::string> variants;
    std::set<std::string> seen;
    auto _add = [&](const std::string& v) {
        if (!v.empty() && seen.insert(v).second) variants.push_back(v);
    };
    auto _add_with_derivatives = [&](const std::string& s) {
        _add(s);
        std::string all_dashed = replace_char(s, '.', '-');
        std::string all_dotted = replace_char(s, '-', '.');
        _add(all_dashed);
        _add(all_dotted);
        _add(dash_to_dot(s));
        _add(dot_to_dash(s));
        _add(dash_to_dot(all_dashed));
        _add(dot_to_dash(all_dotted));
    };
    _add_with_derivatives(model);

    // model.split('/')
    std::vector<std::string> parts;
    {
        std::string cur;
        for (char c : model) {
            if (c == '/') { parts.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        parts.push_back(cur);
    }

    if (parts.size() >= 2) _add_with_derivatives(parts.back());
    if (parts.size() >= 3) {
        std::string joined;
        for (size_t i = 1; i < parts.size(); ++i) {
            if (i > 1) joined.push_back('/');
            joined += parts[i];
        }
        _add_with_derivatives(joined);
    }

    // bare variants (no slash) x known providers
    size_t mark = variants.size();
    for (size_t i = 0; i < mark; ++i) {
        if (variants[i].find('/') == std::string::npos) {
            for (const std::string& p : KNOWN_PROVIDERS()) _add(p + "/" + variants[i]);
        }
    }
    // single-slash variants x known aggregators
    mark = variants.size();
    for (size_t i = 0; i < mark; ++i) {
        int slashes = 0;
        for (char c : variants[i]) if (c == '/') ++slashes;
        if (slashes == 1) {
            for (const std::string& a : KNOWN_AGGREGATORS()) _add(a + "/" + variants[i]);
        }
    }
    return variants;
}

// ---- resolve_per_model_reasoning_effort -------------------------------------
// overrides: Json Obj (else None). Returns json_null() == None.
inline Json resolve_per_model_reasoning_effort(const std::string& model, const Json& overrides) {
    if (model.empty()) return json_null();
    if (overrides.type != Json::Obj || overrides.obj.empty()) return json_null();
    for (const std::string& variant : _canonical_model_variants(model)) {
        const Json* v = json_get(overrides, variant);
        if (!v) continue;
        Json result = parse_reasoning_effort(*v);
        if (result.type != Json::Null) return result;
    }
    return json_null();
}

// ---- resolve_reasoning_config -----------------------------------------------
inline Json resolve_reasoning_config(const Json& cfg, const std::string& model = "") {
    Json c = (cfg.type == Json::Obj) ? cfg : json_obj();
    const Json* agent_raw = json_get(c, "agent");
    Json agent_cfg = (agent_raw && agent_raw->type == Json::Obj) ? *agent_raw : json_obj();

    std::string m = model;
    if (m.empty()) {
        const Json* mc = json_get(c, "model");
        if (mc && mc->type == Json::Str) {
            m = trim_ws(mc->str);
        } else if (mc && mc->type == Json::Obj) {
            const Json* d = json_get(*mc, "default");
            const Json* mm = json_get(*mc, "model");
            if (d && d->type == Json::Str && !trim_ws(d->str).empty()) m = trim_ws(d->str);
            else if (mm && mm->type == Json::Str && !trim_ws(mm->str).empty()) m = trim_ws(mm->str);
        }
    }

    Json overrides = json_obj();
    if (const Json* ov = json_get(agent_cfg, "reasoning_overrides")) {
        if (ov->type == Json::Obj) overrides = *ov;
    }

    Json per_model = resolve_per_model_reasoning_effort(m, overrides);
    if (per_model.type != Json::Null) return per_model;

    const Json* effort = json_get(agent_cfg, "reasoning_effort");
    Json effort_val = effort ? *effort : json_str("");
    Json result = parse_reasoning_effort(effort_val);
    return result;
}

// ---- windows_path_to_wsl ----------------------------------------------------
inline std::optional<std::string> windows_path_to_wsl(const std::string& path) {
    std::string s = trim_ws(path.empty() ? std::string() : path);
    static thread_local std::regex re(R"(^([A-Za-z]):[\\/](.*)$)");
    std::smatch m;
    if (!std::regex_match(s, m, re)) return std::nullopt;
    std::string drive = to_lower(m[1].str());
    std::string tail = replace_char(m[2].str(), '\\', '/');
    return "/mnt/" + drive + "/" + tail;
}

// ---- wsl_unc_path_to_posix --------------------------------------------------
inline std::optional<std::string> wsl_unc_path_to_posix(const std::string& path) {
    std::string normalized = replace_char(trim_ws(path.empty() ? std::string() : path), '/', '\\');
    static thread_local std::regex re(R"(^\\\\wsl(?:\.localhost|\$)\\[^\\]+\\(.*)$)", std::regex::icase);
    std::smatch m;
    if (!std::regex_match(normalized, m, re)) return std::nullopt;
    std::string tail = replace_char(m[1].str(), '\\', '/');
    return tail.empty() ? std::string("/") : "/" + tail;
}

// ---- is_first_party_module --------------------------------------------------
inline bool is_first_party_module(const std::string& name) {
    std::string root;
    if (!name.empty()) {
        auto dot = name.find('.');
        root = (dot == std::string::npos) ? name : name.substr(0, dot);
    }
    if (root.empty()) return false;
    return FIRST_PARTY_MODULE_ROOTS().count(root) > 0 ||
           (root.size() >= 9 && root.compare(0, 9, "daedalus_") == 0);
}

}  // core
}  // dd
#endif
