#pragma once
// dd/core/model_metadata.hpp — port of agent/model_metadata.py PURE extraction island.
//
// Ported (deterministic, no I/O / no network / no Path):
//   _iter_nested_dicts, _coerce_reasonable_int, _extract_first_int,
//   _extract_context_length, _extract_max_completion_tokens, _extract_pricing,
//   _CONTEXT_LENGTH_KEYS / _MAX_COMPLETION_KEYS,
//   estimate_tokens_rough / estimate_messages_tokens_rough / estimate_request_tokens_rough
//   (char//4 metric) + the py_str/py_repr_str serializers they need to mirror
//   Python's str(dict) so the char count matches.
//
// NOT ported (impure boundary): fetch_model_metadata, fetch_endpoint_model_metadata,
//   the *_cache_path/_load_context_cache/get_cached_context_length Path+file cache,
//   _add_model_aliases (mutates a shared cache), parse_prompt_tokens_from_error (regex w/
//   lookaround out of scope), provider-url helpers (_infer_provider_from_url uses urlparse),
//   _normalize_model_version.
//
// Faithfulness notes:
//   * _iter_nested_dicts pre-order: a dict yields itself then recurses into its values in
//     insertion order; a list recurses into its items (the list itself is NOT yielded).
//   * _coerce_reasonable_int mirrors int() semantics: bool->None, str stripped + commas
//     removed then decimal parse (rejects hex/underscores/float-strings like int() does),
//     float truncated toward zero, then range check [min,max].
//   * py_str reproduces Python str() of dict/list/num/bool/None (single-quote repr, ", "
//     and ": " separators). Full repr-escape fidelity covers ASCII + the quote/backslash/
//     \n/\r/\t cases; exotic non-ASCII reprs are OUT of the ported surface (test inputs ASCII).
//   * pricing "value not in (None, '')" exclusion == Null OR empty Str only (0, False, [] kept).
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <cmath>
#include <optional>
#include "dd/core/json.hpp"

namespace dd {
namespace core {

// ---- shortest round-trip float repr / int repr for py_str of Num ----
static inline std::string mm_num_str(double v) {
    if (v < 1e15 && v > -1e15 && v == std::floor(v)) {
        char buf[32]; std::snprintf(buf, sizeof(buf), "%lld", (long long)v);
        return std::string(buf);
    }
    for (int prec = 1; prec <= 17; ++prec) {
        std::ostringstream os; os.precision(prec); os << v;
        std::string s = os.str();
        if (std::stod(s) == v) return s;
    }
    std::ostringstream os; os.precision(17); os << v;
    return os.str();
}

// ---- python str() of a str literal (repr): quote selection + common escapes ----
static inline std::string mm_repr_str(const std::string& s) {
    bool has_sq = s.find('\'') != std::string::npos;
    bool has_dq = s.find('"') != std::string::npos;
    char q = (has_sq && !has_dq) ? '"' : '\'';
    std::string out(1, q);
    for (unsigned char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c == q) { out += '\\'; out += c; }
                else out += (char)c;
        }
    }
    out += q;
    return out;
}

// ---- python str() of a Json value (dict/list/num/bool/None) ----
static std::string py_str(const Json& v) {
    switch (v.type) {
        case Json::Str:  return mm_repr_str(v.str);
        case Json::Num:  return mm_num_str(v.num);
        case Json::Bool: return v.b ? "True" : "False";
        case Json::Null: return "None";
        case Json::Arr: {
            std::string s = "[";
            for (size_t i = 0; i < v.arr.size(); ++i) { if (i) s += ", "; s += py_str(*v.arr[i]); }
            return s + "]";
        }
        case Json::Obj: {
            std::string s = "{";
            for (size_t i = 0; i < v.obj.size(); ++i) {
                if (i) s += ", ";
                s += mm_repr_str(v.obj[i].first) + ": " + py_str(*v.obj[i].second);
            }
            return s + "}";
        }
    }
    return "";
}

// ---- _iter_nested_dicts: pre-order list of Obj node pointers ----
static void iter_nested_dicts(const Json& value, std::vector<const Json*>& out) {
    if (value.type == Json::Obj) {
        out.push_back(&value);
        for (const auto& kv : value.obj) iter_nested_dicts(*kv.second, out);
    } else if (value.type == Json::Arr) {
        for (const auto& e : value.arr) iter_nested_dicts(*e, out);
    }
}

static std::vector<const Json*> iter_nested_dicts_root(const Json& root) {
    std::vector<const Json*> out;
    iter_nested_dicts(root, out);
    return out;
}

// ---- _coerce_reasonable_int ----
static std::optional<long long> coerce_reasonable_int(const Json& v, long long lo = 1024, long long hi = 10000000) {
    if (v.type == Json::Bool) return std::nullopt;                 // bool excluded
    std::string sval;   // numeric-as-string, "" if not applicable
    bool have_num = false;
    if (v.type == Json::Num) {
        double r = (v.num < 0) ? std::ceil(v.num) : std::floor(v.num);   // int(float) trunc toward zero
        if (r < (double)lo || r > (double)hi) {
            // still need exact int(); compare as integer
        }
        have_num = true;
        // store as integral string
        { char buf[32]; std::snprintf(buf, sizeof(buf), "%lld", (long long)r); sval = buf; }
    } else if (v.type == Json::Str) {
        // strip (Python strip strips all whitespace), remove commas, then int()
        std::string t = v.str;
        // strip leading/trailing whitespace
        size_t a = 0, b = t.size();
        auto ws = [](char c){ return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v'; };
        while (a < b && ws(t[a])) ++a;
        while (b > a && ws(t[b-1])) --b;
        t = t.substr(a, b - a);
        // remove commas
        std::string cleaned;
        for (char c : t) if (c != ',') cleaned += c;
        // int(cleaned): decimal, optional +/-; reject empty / non-digits
        if (cleaned.empty()) return std::nullopt;
        size_t i = 0; bool neg = false;
        if (cleaned[0] == '+' || cleaned[0] == '-') { neg = (cleaned[0]=='-'); i = 1; }
        if (i >= cleaned.size()) return std::nullopt;      // lone sign
        for (; i < cleaned.size(); ++i) if (cleaned[i] < '0' || cleaned[i] > '9') return std::nullopt;
        // value range guard (avoid overflow): reject absurd length
        if (cleaned.size() > 19) return std::nullopt;
        long long val = std::stoll(cleaned);
        if (neg) val = -val;
        if (val < lo || val > hi) return std::nullopt;
        return val;
    } else {
        return std::nullopt;   // Arr/Obj/Null
    }
    if (have_num) {
        long long val = std::stoll(sval);
        if (val < lo || val > hi) return std::nullopt;
        return val;
    }
    return std::nullopt;
}

// ---- _extract_first_int ----
static std::optional<long long> extract_first_int(const Json& payload, const std::vector<std::string>& keys) {
    std::vector<std::string> keyset;
    for (const auto& k : keys) { std::string lk = k; for (auto& c : lk) c = (char)std::tolower((unsigned char)c); keyset.push_back(lk); }
    for (const Json* m : iter_nested_dicts_root(payload)) {
        for (const auto& kv : m->obj) {
            std::string lk = kv.first;
            for (auto& c : lk) c = (char)std::tolower((unsigned char)c);
            bool hit = false;
            for (const auto& k : keyset) if (k == lk) { hit = true; break; }
            if (!hit) continue;
            auto coerced = coerce_reasonable_int(*kv.second);
            if (coerced) return coerced;
        }
    }
    return std::nullopt;
}

static const std::vector<std::string>& CONTEXT_LENGTH_KEYS() {
    static const std::vector<std::string> v = {
        "context_length","context_window","max_context_length","max_position_embeddings",
        "max_model_len","max_input_tokens","max_sequence_length","max_seq_len","n_ctx_train","n_ctx"};
    return v;
}
static const std::vector<std::string>& MAX_COMPLETION_KEYS() {
    static const std::vector<std::string> v = {"max_completion_tokens","max_output_tokens","max_tokens"};
    return v;
}

static std::optional<long long> extract_context_length(const Json& payload) {
    return extract_first_int(payload, CONTEXT_LENGTH_KEYS());
}
static std::optional<long long> extract_max_completion_tokens(const Json& payload) {
    return extract_first_int(payload, MAX_COMPLETION_KEYS());
}

// ---- _extract_pricing -> Json obj (target keys in insertion order), or {} ----
static Json extract_pricing(const Json& payload) {
    // alias_map insertion order
    const char* targets[5] = {"prompt","completion","request","cache_read","cache_write"};
    const std::vector<std::vector<std::string>> groups = {
        {"prompt","input","input_cost_per_token","prompt_token_cost"},
        {"completion","output","output_cost_per_token","completion_token_cost"},
        {"request","request_cost"},
        {"cache_read","cached_prompt","input_cache_read","cache_read_cost_per_token"},
        {"cache_write","cache_creation","input_cache_write","cache_write_cost_per_token"},
    };
    auto excluded = [](const Json& x){ return x.type == Json::Null || (x.type == Json::Str && x.str.empty()); };
    for (const Json* m : iter_nested_dicts_root(payload)) {
        // normalized: lower(key) -> value ptr (last wins on collision)
        std::map<std::string, const Json*> norm;
        for (const auto& kv : m->obj) {
            std::string lk = kv.first;
            for (auto& c : lk) c = (char)std::tolower((unsigned char)c);
            norm[lk] = kv.second.get();
        }
        bool any = false;
        for (const auto& g : groups) for (const auto& al : g) if (norm.find(al) != norm.end()) { any = true; break; }
        if (!any) continue;
        Json pricing = json_obj();
        for (size_t t = 0; t < 5; ++t) {
            for (const auto& al : groups[t]) {
                auto it = norm.find(al);
                if (it != norm.end() && !excluded(*it->second)) {
                    obj_set(pricing, targets[t], json_deep_copy(*it->second));
                    break;
                }
            }
        }
        if (!pricing.obj.empty()) return pricing;
    }
    return json_obj();
}

// ---- rough token estimators (char//4; ASCII: bytes == code points) ----
static long long estimate_tokens_rough(const std::string& text) {
    if (text.empty()) return 0;
    return (long long)text.size() / 4;
}
static long long estimate_messages_tokens_rough(const Json& messages) {
    if (messages.type != Json::Arr) return 0;
    long long total = 0;
    for (const auto& msg : messages.arr) total += (long long)py_str(*msg).size();
    return total / 4;
}
static long long estimate_request_tokens_rough(const Json& messages, const std::string& system_prompt, const Json& tools) {
    long long total = 0;
    if (!system_prompt.empty()) total += (long long)system_prompt.size();
    if (messages.type == Json::Arr) for (const auto& msg : messages.arr) total += (long long)py_str(*msg).size();
    if (tools.type == Json::Arr && !tools.arr.empty()) total += (long long)py_str(tools).size();
    return total / 4;
}

} // core
} // dd
