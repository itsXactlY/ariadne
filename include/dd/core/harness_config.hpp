#pragma once
// dd/core/harness_config.hpp — port of agent/harness_config.py PURE parts.
//
// Ported (deterministic, no I/O / no env / no yaml):
//   ConfigProblem (+__str__), ConfigError (strict throw), _type_ok (scalar kinds
//   only), Section.build (known-field type checks + unknown-key report + defaults
//   + nested sections), Section._one_of, every section's _validate, the section
//   field tables + defaults (Watermarks, Compression, Context, Soak, Memory,
//   AuxTask, Skills, ToolSearch, MCPServer, Agent), HarnessConfig.from_dict
//   (root assembly + dead/unknown/type/value problems + strict ConfigError),
//   MCPServer.disabled_tools.
//
// NOT ported (impure boundary): HarnessConfig.load (yaml + os.environ + Path),
//   _main (argparse), _sample_tree/render_sample (yaml render), the `get`/`aux`/
//   `server`/`enabled_servers`/`report`/`is_clean` accessors (thin over built
//   state; report == str-of-problems which we expose directly).
//
// Faithfulness notes:
//   * Problem ORDER reproduces Python: per-section field order (type problems),
//     then unknown-key pass (raw insertion order), then _validate (value problems);
//     nested-section validate fires during the field pass.
//   * type-problem detail `expected X, got Y (repr) — using the default`; em-dash
//     embedded as UTF-8 bytes.
//   * scalar repr: str -> 'x'; the only type-problems exercised use string values,
//     so int-vs-float repr ambiguity is sidestepped by the test inputs.
//   * number repr (value problems) uses json_dump formatting (integer-valued ->
//     "900"); ground truth feeds integer literals so Python f"{5}" == "5".
#include <string>
#include <vector>
#include <set>
#include <sstream>
#include <cmath>
#include <functional>
#include <algorithm>
#include <cstdio>
#include "dd/core/json.hpp"

namespace dd {
namespace core {

// ---- problem model ----
struct Problem {
    std::string path;
    std::string kind;   // unknown | type | value | dead
    std::string detail;
    std::string to_str() const { return "[" + kind + "] " + path + ": " + detail; }
};

// ---- number repr mirroring json_dump (integer-valued -> long long) ----
static inline std::string num_str(double v) {
    if (v < 1e15 && v > -1e15 && v == std::floor(v)) {
        char buf[32]; std::snprintf(buf, sizeof(buf), "%lld", (long long)v);
        return std::string(buf);
    }
    // shortest representation that round-trips (matches Python repr/str of float)
    for (int prec = 1; prec <= 17; ++prec) {
        std::ostringstream os; os.precision(prec); os << v;
        std::string s = os.str();
        if (std::stod(s) == v) return s;
    }
    std::ostringstream os; os.precision(17); os << v;
    return os.str();
}

// ---- python-ish reprs ----
static inline std::string qstr(const std::string& s) { return "'" + s + "'"; }
static inline std::string tuple_repr(const std::vector<std::string>& a) {
    std::string s = "(";
    for (size_t i = 0; i < a.size(); ++i) { if (i) s += ", "; s += qstr(a[i]); }
    return s + ")";
}
// value repr (scalar). Str -> 'x'; Num -> num_str; Bool -> True/False; Null -> None.
static inline std::string py_repr(const Json& v) {
    switch (v.type) {
        case Json::Str:  return qstr(v.str);
        case Json::Num:  return num_str(v.num);
        case Json::Bool: return v.b ? "True" : "False";
        case Json::Null: return "None";
        default:         return json_dump(v);
    }
}
// python type().__name__ for the raw Json value
static inline std::string py_type_name(const Json& v) {
    switch (v.type) {
        case Json::Str:  return "str";
        case Json::Bool: return "bool";
        case Json::Null: return "NoneType";
        case Json::Arr:  return "list";
        case Json::Obj:  return "dict";
        case Json::Num:  return (v.num == std::floor(v.num)) ? "int" : "float";
    }
    return "object";
}

// ---- kind constants for scalar type checks ----
enum Kind { K_INT = 0, K_FLOAT = 1, K_BOOL = 2, K_STR = 3, K_STRLIST = 4, K_DICT = 5, K_SECT = 6 };

static inline bool type_ok(const Json& v, int k) {
    switch (k) {
        case K_INT:    return v.type == Json::Num && v.num == std::floor(v.num);
        case K_FLOAT:  return v.type == Json::Num;
        case K_BOOL:   return v.type == Json::Bool;
        case K_STR:    return v.type == Json::Str;
        case K_DICT:   return v.type == Json::Obj;
        case K_STRLIST:
            if (v.type != Json::Arr) return false;
            for (const auto& e : v.arr) if (e->type != Json::Str) return false;
            return true;
        default: return true; // K_SECT handled elsewhere
    }
}
// expected-type __name__ for a field kind (getattr(tp,'__name__'))
static inline std::string tp_name(int k) {
    switch (k) {
        case K_INT: return "int"; case K_FLOAT: return "float"; case K_BOOL: return "bool";
        case K_STR: return "str"; case K_STRLIST: return "list"; case K_DICT: return "dict";
        default: return "object";
    }
}

// ---- section field table ----
struct Field {
    std::string name;
    int kind;
    std::string tpn;                              // expected-type name
    Json dflt;                                    // default value (ignored for K_SECT)
    std::function<Json(const Json&, std::string, std::vector<Problem>&)> sub; // K_SECT builder
};

using ProblemVec = std::vector<Problem>;

// generic Section.build: defaults -> accepted-from-raw (type checks) -> unknown pass -> validate
static Json build_generic(const Json& raw, const std::vector<Field>& fs,
                          const std::string& path, ProblemVec& probs,
                          const std::function<void(const Json&, const std::string&, ProblemVec&)>& validate) {
    Json r = (raw.type == Json::Obj) ? raw : json_obj();
    // "expected a mapping, got X" for non-mapping non-null raw
    if (raw.type != Json::Obj && raw.type != Json::Null) {
        probs.push_back({path, "type", "expected a mapping, got " + py_type_name(raw)});
    }
    Json b = json_obj();
    std::set<std::string> known;
    for (const auto& f : fs) known.insert(f.name);

    for (const auto& f : fs) {
        if (f.kind == K_SECT) {
            const Json* v = json_get(r, f.name);
            if (v && v->type != Json::Null) {
                obj_set(b, f.name, f.sub(*v, path + "." + f.name, probs));
            } else {
                ProblemVec tmp;
                obj_set(b, f.name, f.sub(json_null(), path + "." + f.name, tmp));
            }
            continue;
        }
        obj_set(b, f.name, f.dflt); // default first (missing / null / bad-type keep it)
        const Json* v = json_get(r, f.name);
        if (!v) continue;
        if (v->type == Json::Null) continue;   // yaml null == not configured
        if (!type_ok(*v, f.kind)) {
            probs.push_back({path + "." + f.name, "type",
                "expected " + f.tpn + ", got " + py_type_name(*v) + " (" + py_repr(*v) + ") \xE2\x80\x94 using the default"});
            continue;
        }
        obj_set(b, f.name, *v);
    }
    // unknown keys (raw insertion order)
    if (r.type == Json::Obj) {
        for (const auto& kv : r.obj) {
            if (known.find(kv.first) == known.end())
                probs.push_back({path + "." + kv.first, "unknown", "nothing in the harness reads this"});
        }
    }
    if (validate) validate(b, path, probs);
    return b;
}

// ---- shared value helpers ----
static inline void one_of(const Json& b, const std::string& path, const std::string& name,
                          const std::vector<std::string>& allowed, ProblemVec& probs) {
    const Json* v = json_get(b, name);
    if (!v || v->type != Json::Str) return;
    bool ok = false;
    for (const auto& a : allowed) if (a == v->str) { ok = true; break; }
    if (!ok) probs.push_back({path + "." + name, "value", qstr(v->str) + " not in " + tuple_repr(allowed)});
}

// ---- section builders ----
static inline void validate_watermarks(const Json& b, const std::string& path, ProblemVec& probs) {
    const Json* lo = json_get(b, "low"), *hi = json_get(b, "high"), *hd = json_get(b, "hard");
    double l = lo ? lo->num : 0.6, h = hi ? hi->num : 0.8, d = hd ? hd->num : 0.9;
    if (!(0 < l && l <= h && h <= d && d <= 1.0))
        probs.push_back({path, "value", "must satisfy 0 < low <= high <= hard <= 1, got "
            + num_str(l) + "/" + num_str(h) + "/" + num_str(d)});
}
static Json build_watermarks(const Json& raw, const std::string& path, ProblemVec& probs) {
    std::vector<Field> fs = {
        {"low", K_FLOAT, "float", json_num(0.6), nullptr},
        {"high", K_FLOAT, "float", json_num(0.8), nullptr},
        {"hard", K_FLOAT, "float", json_num(0.9), nullptr},
    };
    return build_generic(raw, fs, path, probs, validate_watermarks);
}
static Json build_soak(const Json& raw, const std::string& path, ProblemVec& probs) {
    std::vector<Field> fs = {
        {"enabled", K_BOOL, "bool", json_bool(true), nullptr},
        {"window_turns", K_INT, "int", json_num(8), nullptr},
        {"state_reinject_every", K_INT, "int", json_num(2), nullptr},
    };
    return build_generic(raw, fs, path, probs, nullptr);
}

static inline void validate_compression(const Json& b, const std::string& path, ProblemVec& probs) {
    one_of(b, path, "engine", {"retrieval", "compressor"}, probs);
    const Json* th = json_get(b, "threshold");
    double t = th ? th->num : 0.5;
    if (!(0.0 < t && t <= 1.0))
        probs.push_back({path + ".threshold", "value", num_str(t) + " must be in (0, 1]"});
}
static Json build_compression(const Json& raw, const std::string& path, ProblemVec& probs) {
    std::vector<Field> fs = {
        {"enabled", K_BOOL, "bool", json_bool(true), nullptr},
        {"threshold", K_FLOAT, "float", json_num(0.5), nullptr},
        {"max_tokens", K_INT, "int", json_num(32000), nullptr},
        {"engine", K_STR, "str", json_str("retrieval"), nullptr},
        {"fast_layer_tokens", K_INT, "int", json_num(0), nullptr},
        {"hot_swap_prep_ratio", K_FLOAT, "float", json_num(0.0), nullptr},
        {"protect_last_n", K_INT, "int", json_num(6), nullptr},
        {"protect_first_n", K_INT, "int", json_num(4), nullptr},
        {"target_ratio", K_FLOAT, "float", json_num(0.1), nullptr},
        {"tail_budget", K_INT, "int", json_num(3200), nullptr},
        {"summary_model", K_STR, "str", json_str(""), nullptr},
        {"summary_provider", K_STR, "str", json_str(""), nullptr},
        {"summary_base_url", K_STR, "str", json_str(""), nullptr},
        {"watermarks", K_SECT, "Watermarks", json_null(), build_watermarks},
    };
    return build_generic(raw, fs, path, probs, validate_compression);
}

static Json build_context(const Json& raw, const std::string& path, ProblemVec& probs) {
    std::vector<Field> fs = {
        {"reasoning_window", K_INT, "int", json_num(2), nullptr},
        {"tool_group_ttl", K_INT, "int", json_num(3), nullptr},
        {"payload_tape", K_BOOL, "bool", json_bool(true), nullptr},
    };
    return build_generic(raw, fs, path, probs, nullptr);
}

static inline void validate_memory(const Json& b, const std::string& path, ProblemVec& probs) {
    one_of(b, path, "provider", {"mazemaker", "mcp", "builtin", ""}, probs);
    const Json* pt = json_get(b, "prefetch_timeout_s");
    double p = pt ? pt->num : 30.0;
    if (p < 10.0)
        probs.push_back({path + ".prefetch_timeout_s", "value",
            num_str(p) + "s is under pod recall latency (5-19s under GPU contention); the join times out and the turn gets no memory context"});
}
static Json build_memory(const Json& raw, const std::string& path, ProblemVec& probs) {
    std::vector<Field> fs = {
        {"provider", K_STR, "str", json_str("mazemaker"), nullptr},
        {"prefetch_timeout_s", K_FLOAT, "float", json_num(30.0), nullptr},
        {"memory_enabled", K_BOOL, "bool", json_bool(true), nullptr},
        {"user_profile_enabled", K_BOOL, "bool", json_bool(true), nullptr},
        {"memory_char_limit", K_INT, "int", json_num(0), nullptr},
        {"user_char_limit", K_INT, "int", json_num(0), nullptr},
        {"flush_max_chars", K_INT, "int", json_num(0), nullptr},
        {"flush_max_tokens", K_INT, "int", json_num(0), nullptr},
        {"flush_timeout", K_FLOAT, "float", json_num(0.0), nullptr},
        {"soak", K_SECT, "Soak", json_null(), build_soak},
    };
    return build_generic(raw, fs, path, probs, validate_memory);
}

static Json build_auxtask(const Json& raw, const std::string& path, ProblemVec& probs) {
    std::vector<Field> fs = {
        {"provider", K_STR, "str", json_str(""), nullptr},
        {"model", K_STR, "str", json_str(""), nullptr},
        {"base_url", K_STR, "str", json_str(""), nullptr},
        {"api_key", K_STR, "str", json_str(""), nullptr},
        {"timeout", K_FLOAT, "float", json_num(120.0), nullptr},
        {"download_timeout", K_FLOAT, "float", json_num(30.0), nullptr},
        {"api_mode", K_STR, "str", json_str(""), nullptr},
        {"effort", K_STR, "str", json_str(""), nullptr},
        {"reasoning_effort", K_STR, "str", json_str(""), nullptr},
        {"context_length", K_INT, "int", json_num(0), nullptr},
        {"extra_body", K_DICT, "dict", json_obj(), nullptr},
    };
    return build_generic(raw, fs, path, probs, nullptr);
}
static Json build_skills(const Json& raw, const std::string& path, ProblemVec& probs) {
    std::vector<Field> fs = {
        {"enabled", K_BOOL, "bool", json_bool(true), nullptr},
        {"auto_route", K_BOOL, "bool", json_bool(true), nullptr},
        {"external_dirs", K_STRLIST, "list", json_arr(), nullptr},
        {"disabled", K_STRLIST, "list", json_arr(), nullptr},
    };
    return build_generic(raw, fs, path, probs, nullptr);
}
static Json build_tool_search(const Json& raw, const std::string& path, ProblemVec& probs) {
    std::vector<Field> fs = {
        {"enabled", K_BOOL, "bool", json_bool(true), nullptr},
        {"threshold", K_INT, "int", json_num(0), nullptr},
        {"always_load", K_STRLIST, "list", json_arr(), nullptr},
    };
    return build_generic(raw, fs, path, probs, nullptr);
}

static inline void validate_mcp(const Json& b, const std::string& path, ProblemVec& probs) {
    one_of(b, path, "transport", {"http", "stdio"}, probs);
    const Json* en = json_get(b, "enabled"), *tr = json_get(b, "transport"), *ur = json_get(b, "url"), *cm = json_get(b, "command");
    bool enabled = en && en->type == Json::Bool && en->b;
    std::string transport = tr && tr->type == Json::Str ? tr->str : "http";
    std::string url = ur && ur->type == Json::Str ? ur->str : "";
    std::string command = cm && cm->type == Json::Str ? cm->str : "";
    if (enabled && transport == "http" && url.empty())
        probs.push_back({path + ".url", "value", "http transport with no url"});
    if (enabled && transport == "stdio" && command.empty())
        probs.push_back({path + ".command", "value", "stdio transport with no command"});
}
static Json build_mcp(const Json& raw, const std::string& path, ProblemVec& probs) {
    std::vector<Field> fs = {
        {"enabled", K_BOOL, "bool", json_bool(false), nullptr},
        {"transport", K_STR, "str", json_str("http"), nullptr},
        {"url", K_STR, "str", json_str(""), nullptr},
        {"command", K_STR, "str", json_str(""), nullptr},
        {"args", K_STRLIST, "list", json_arr(), nullptr},
        {"description", K_STR, "str", json_str(""), nullptr},
        {"tools", K_DICT, "dict", json_obj(), nullptr},
    };
    return build_generic(raw, fs, path, probs, validate_mcp);
}

static inline void validate_agent(const Json& b, const std::string& path, ProblemVec& probs) {
    one_of(b, path, "tool_use_enforcement", {"auto", "required", "off"}, probs);
    const Json* wj = json_get(b, "gateway_timeout_warning"), *tj = json_get(b, "gateway_timeout");
    long long w = wj ? (long long)wj->num : 900, t = tj ? (long long)tj->num : 1800;
    if (w >= t)
        probs.push_back({path + ".gateway_timeout_warning", "value",
            num_str((double)w) + "s >= gateway_timeout " + num_str((double)t) + "s \xE2\x80\x94 the warning can never fire"});
}
static Json build_agent(const Json& raw, const std::string& path, ProblemVec& probs) {
    std::vector<Field> fs = {
        {"max_turns", K_INT, "int", json_num(400), nullptr},
        {"gateway_timeout", K_INT, "int", json_num(1800), nullptr},
        {"gateway_timeout_warning", K_INT, "int", json_num(900), nullptr},
        {"gateway_notify_interval", K_INT, "int", json_num(600), nullptr},
        {"restart_drain_timeout", K_INT, "int", json_num(60), nullptr},
        {"tool_use_enforcement", K_STR, "str", json_str("auto"), nullptr},
        {"service_tier", K_STR, "str", json_str(""), nullptr},
        {"reasoning_effort", K_STR, "str", json_str(""), nullptr},
        {"reasoning_auto", K_BOOL, "bool", json_bool(true), nullptr},
        {"reasoning_floor", K_STR, "str", json_str(""), nullptr},
        {"system_prompt", K_STR, "str", json_str(""), nullptr},
        {"verbose", K_BOOL, "bool", json_bool(false), nullptr},
    };
    return build_generic(raw, fs, path, probs, validate_agent);
}

// MCPServer.disabled_tools: sorted names where tools[name] is a dict with enabled is False
static std::vector<std::string> mcp_disabled_tools(const Json& tools) {
    std::vector<std::string> out;
    if (tools.type == Json::Obj) {
        for (const auto& kv : tools.obj) {
            if (kv.second->type == Json::Obj) {
                const Json* en = json_get(*kv.second, "enabled");
                if (en && en->type == Json::Bool && en->b == false) out.push_back(kv.first);
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ---- root: HarnessConfig.from_dict (strict==True -> ConfigError) ----
static void from_dict(const Json& raw, ProblemVec& probs, bool strict) {
    std::vector<Problem>& p = probs;
    static const std::vector<std::string> SECTION_NAMES = {"compression", "context", "memory", "skills", "tool_search", "agent"};
    static const std::set<std::string> PASSTHROUGH = {
        "model", "providers", "credential_pool_strategies", "toolsets", "terminal",
        "browser", "checkpoints", "smart_model_routing", "display", "privacy",
        "tts", "stt", "voice", "human_delay", "delegation", "prefill_messages_file",
        "timezone", "discord", "approvals", "command_allowlist", "security", "cron",
        "logging", "curator", "_config_version", "network", "session_reset", "pony",
        "routing", "tools", "fallback_providers", "honcho", "personalities",
        "quick_commands", "whatsapp", "file_read_max_chars",
    };
    const Json nullv = json_null();
    auto getraw = [&](const std::string& key) -> const Json& {
        if (raw.type == Json::Obj) {
            const Json* v = json_get(raw, key);
            if (v) return *v;
        }
        return nullv;
    };
    for (const auto& sec : SECTION_NAMES) {
        const Json& v = getraw(sec);
        if (sec == "compression") build_compression(v, "compression", p);
        else if (sec == "context") build_context(v, "context", p);
        else if (sec == "memory") build_memory(v, "memory", p);
        else if (sec == "skills") build_skills(v, "skills", p);
        else if (sec == "tool_search") build_tool_search(v, "tool_search", p);
        else if (sec == "agent") build_agent(v, "agent", p);
    }
    // auxiliary
    const Json& aux = getraw("auxiliary");
    if (aux.type == Json::Obj) {
        for (const auto& kv : aux.obj) build_auxtask(*kv.second, "auxiliary." + kv.first, p);
    }
    // mcp_servers
    const Json& srv = getraw("mcp_servers");
    if (srv.type == Json::Obj) {
        for (const auto& kv : srv.obj) build_mcp(*kv.second, "mcp_servers." + kv.first, p);
    }
    // dead keys at root
    if (raw.type == Json::Obj) {
        std::set<std::string> skip(SECTION_NAMES.begin(), SECTION_NAMES.end());
        skip.insert("mcp_servers"); skip.insert("auxiliary");
        for (const auto& kv : raw.obj) {
            if (skip.find(kv.first) == skip.end() && PASSTHROUGH.find(kv.first) == PASSTHROUGH.end())
                p.push_back({kv.first, "dead", "no code path reads this section"});
        }
    }
    if (strict) {
        std::vector<std::string> hard;
        for (const auto& pr : p) if (pr.kind == "type" || pr.kind == "value") hard.push_back(pr.to_str());
        if (!hard.empty()) {
            std::string msg;
            for (size_t i = 0; i < hard.size(); ++i) { if (i) msg += "; "; msg += hard[i]; }
            throw std::runtime_error(msg);
        }
    }
}

} // core
} // dd
