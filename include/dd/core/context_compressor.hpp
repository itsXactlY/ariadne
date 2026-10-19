#pragma once
// Port of context_compressor.py message-graph core: ContextConfig (compression-relevant
// numeric subset), _prune_old_tool_results, _prune_floor, live_window_chars,
// _adaptive_tail, _payload_chars, _tool_call_names, _is_offloaded, _tool_call_subjects,
// _describe_arguments, _with_summary_prefix, _get_tool_call_id, _sanitize_tool_pairs,
// _align_boundary_forward/_backward, _find_tail_cut_by_tokens, compress.
// The LLM summarizer is OUT of scope (external): compress injects summary_fn mirroring
// _generate_summary (returns "" == Python None: middle turns dropped without a summary).
// estimate_messages_tokens_rough / context-length detection are OUT (model/network-bound):
// threshold_tokens, tail_token_budget, live_window_messages are passed in as config; the
// rough estimate only ever feeds logging, never the return path, so it is omitted.
#include <algorithm>
#include <functional>
#include <set>
#include <string>
#include <vector>
#include "dd/core/json.hpp"

namespace dd {
namespace core {

using core::Json;
using core::JsonSP;

// ---- module constants ----
inline const std::string SUMMARY_PREFIX =
    "[CONTEXT COMPACTION] Earlier turns in this conversation were compacted "
    "to save context space. The summary below describes work that was "
    "already completed, and the current session state may still reflect "
    "that work (for example, files may already be changed). Use the summary "
    "and the current state to continue from where things left off, and "
    "avoid repeating work:";
inline const std::string LEGACY_SUMMARY_PREFIX = "[CONTEXT SUMMARY]:";
inline const std::string _PRUNED_TOOL_PLACEHOLDER = "[Old tool output cleared to save context space]";
inline constexpr long long _CHARS_PER_TOKEN = 4;
inline constexpr long long _PRUNE_MIN_CHARS = 2000;
inline constexpr long long _PRUNE_MIN_CHARS_UNDER_PRESSURE = 800;
inline const double _PRUNE_PRESSURE_RATIO = 0.6;
inline constexpr long long _LIVE_WINDOW_MESSAGES = 12;
inline constexpr long long _LIVE_WINDOW_FLOOR = 4;
inline const double _LIVE_WINDOW_BUDGET_RATIO = 0.5;
inline const std::string _OFFLOAD_PREFIX = "[offloaded: ";

// ---- ContextConfig (numeric knobs the message-graph algorithm reads) ----
struct ContextConfig {
    long long protect_first_n = 3;
    long long protect_last_n = 20;
    long long threshold_tokens = 0;
    long long tail_token_budget = 0;
    long long live_window_messages = _LIVE_WINDOW_MESSAGES;
    long long compression_count = 0;
};

// ---- message-graph access helpers ----
// role / string-valued field getter (mirrors msg.get(key, default) for Str-valued keys).
inline std::string get_str(const Json& msg, const std::string& key, const std::string& def) {
    const Json* v = core::json_get(msg, key);
    if (!v || v->type != Json::Str) return def;
    return v->str;
}

// Python truthiness of a Json value.
inline bool py_truthy(const Json* v) {
    if (!v) return false;
    switch (v->type) {
        case Json::Null: return false;
        case Json::Bool: return v->b;
        case Json::Num:  return v->num != 0.0;
        case Json::Str:  return !v->str.empty();
        case Json::Arr:  return !v->arr.empty();
        case Json::Obj:  return !v->obj.empty();
    }
    return true;
}

// Python str.strip() over ASCII whitespace.
inline std::string strip_ascii(const std::string& s) {
    auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
    size_t a = 0, b = s.size();
    while (a < b && ws(s[a])) ++a;
    while (b > a && ws(s[b - 1])) --b;
    return s.substr(a, b - a);
}

// Python str.lstrip() over ASCII whitespace.
inline std::string lstrip_ascii(const std::string& s) {
    auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
    size_t a = 0, b = s.size();
    while (a < b && ws(s[a])) ++a;
    return s.substr(a, b - a);
}

// " ".join(value.split()): collapse whitespace runs to single spaces, drop edge spaces.
inline std::string collapse_ws(const std::string& s) {
    auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
    std::string out;
    bool pending = false;
    for (char c : s) {
        if (ws(c)) { pending = !out.empty(); continue; }
        if (pending) { out += ' '; pending = false; }
        out += c;
    }
    return out;
}

// ---- _payload_chars ----
inline long long payload_chars(const Json& msgs) {
    long long total = 0;
    for (const auto& msp : msgs.arr) {
        const Json& msg = *msp;
        if (msg.type != Json::Obj) continue;
        total += (long long)get_str(msg, "content", "").size();
        const Json* tc = core::json_get(msg, "tool_calls");
        if (tc && tc->type == Json::Arr) {
            for (const auto& csp : tc->arr) {
                const Json& call = *csp;
                if (call.type != Json::Obj) continue;
                const Json* fn = core::json_get(call, "function");
                std::string args = (fn && fn->type == Json::Obj) ? get_str(*fn, "arguments", "") : "";
                total += (long long)args.size();
            }
        }
    }
    return total;
}

// ---- live_window_chars ----
inline long long live_window_chars(long long threshold_tokens) {
    if (threshold_tokens <= 0) return 0;
    return (long long)((double)threshold_tokens * 4.0 * _LIVE_WINDOW_BUDGET_RATIO);
}

// ---- _prune_floor ----
inline long long prune_floor(long long current_tokens, long long threshold_tokens) {
    if (threshold_tokens <= 0) return _PRUNE_MIN_CHARS;
    if ((double)current_tokens >= (double)threshold_tokens * _PRUNE_PRESSURE_RATIO)
        return _PRUNE_MIN_CHARS_UNDER_PRESSURE;
    return _PRUNE_MIN_CHARS;
}

// ---- _adaptive_tail ----
inline long long adaptive_tail(const Json& msgs, long long live_window_messages,
                               long long live_window_budget) {
    const long long n = (long long)msgs.arr.size();
    long long tail = std::min(live_window_messages, n);
    if (live_window_budget <= 0) return tail;
    // messages[-tail:] payload
    while (tail > _LIVE_WINDOW_FLOOR) {
        Json slice = core::json_arr();
        for (long long i = n - tail; i < n; ++i)
            core::arr_push(slice, *msgs.arr[(size_t)i]);
        if (payload_chars(slice) > live_window_budget) --tail;
        else break;
    }
    return tail;
}

// ---- _prune_old_tool_results ----
std::pair<Json, long long> prune_old_tool_results(const Json& msgs, long long protect_tail_count) {
    if (msgs.arr.empty()) return {core::json_deep_copy(msgs), 0};
    Json result = core::json_arr();
    for (const auto& msp : msgs.arr) core::arr_push(result, core::json_deep_copy(*msp));
    long long pruned = 0;
    const long long prune_boundary = (long long)result.arr.size() - protect_tail_count;
    for (long long i = 0; i < prune_boundary; ++i) {
        Json& msg = *result.arr[(size_t)i];
        if (get_str(msg, "role", "") != "tool") continue;
        std::string content = get_str(msg, "content", "");
        if (content.empty() || content == _PRUNED_TOOL_PLACEHOLDER) continue;
        if ((long long)content.size() > 200) {
            core::obj_set(msg, "content", core::json_str(_PRUNED_TOOL_PLACEHOLDER));
            ++pruned;
        }
    }
    return {std::move(result), pruned};
}

// ---- _tool_call_names ----
inline Json tool_call_names(const Json& msgs) {
    Json names = core::json_obj();
    for (const auto& msp : msgs.arr) {
        const Json* tc = core::json_get(*msp, "tool_calls");
        if (!tc || tc->type != Json::Arr) continue;
        for (const auto& csp : tc->arr) {
            const Json& call = *csp;
            if (call.type != Json::Obj) continue;
            if (!core::py_truthy(core::json_get(call, "id"))) continue;
            std::string id = get_str(call, "id", "");
            const Json* fn = core::json_get(call, "function");
            std::string name = (fn && fn->type == Json::Obj) ? get_str(*fn, "name", "") : "";
            if (name.empty()) name = "tool";
            core::obj_set(names, id, core::json_str(name));
        }
    }
    return names;
}

// ---- _is_offloaded ----
inline bool is_offloaded(const std::string& content) {
    return content == _PRUNED_TOOL_PLACEHOLDER || content.rfind(_OFFLOAD_PREFIX, 0) == 0;
}

// ---- _describe_arguments ----
inline std::string describe_arguments(const Json* raw) {
    if (!raw || raw->type != Json::Str) return "";
    if (strip_ascii(raw->str).empty()) return "";
    Json parsed;
    try {
        parsed = core::json_parse(raw->str);
    } catch (...) {
        return "";
    }
    if (parsed.type != Json::Obj) return "";
    static const std::vector<std::string> SUBJECT_KEYS = {
        "file_path", "path", "command", "query", "url", "name"};
    for (const std::string& key : SUBJECT_KEYS) {
        const Json* v = core::json_get(parsed, key);
        if (v && v->type == Json::Str && !strip_ascii(v->str).empty()) {
            std::string flat = collapse_ws(v->str);
            if ((long long)flat.size() <= 120) return flat;
            return flat.substr(0, 117) + "...";
        }
    }
    return "";
}

// ---- _tool_call_subjects ----
inline Json tool_call_subjects(const Json& msgs) {
    Json subjects = core::json_obj();
    for (const auto& msp : msgs.arr) {
        const Json* tc = core::json_get(*msp, "tool_calls");
        if (!tc || tc->type != Json::Arr) continue;
        for (const auto& csp : tc->arr) {
            const Json& call = *csp;
            if (call.type != Json::Obj) continue;
            if (!core::py_truthy(core::json_get(call, "id"))) continue;
            std::string id = get_str(call, "id", "");
            const Json* fn = core::json_get(call, "function");
            const Json* raw = (fn && fn->type == Json::Obj) ? core::json_get(*fn, "arguments") : nullptr;
            core::obj_set(subjects, id, core::json_str(describe_arguments(raw)));
        }
    }
    return subjects;
}

// ---- _with_summary_prefix ----
inline std::string with_summary_prefix(const std::string& summary) {
    std::string text = strip_ascii(summary);
    for (const std::string& prefix : {LEGACY_SUMMARY_PREFIX, SUMMARY_PREFIX}) {
        if (text.rfind(prefix, 0) == 0) {
            text = lstrip_ascii(text.substr(prefix.size()));
            break;
        }
    }
    if (text.empty()) return SUMMARY_PREFIX;
    return SUMMARY_PREFIX + "\n" + text;
}

// ---- _get_tool_call_id ----
inline std::string get_tool_call_id(const Json& tc) {
    if (tc.type != Json::Obj) return "";
    const Json* v = core::json_get(tc, "id");
    if (!v) return "";
    if (v->type == Json::Str) return v->str;
    return "";  // Python returns the raw value (None); unreachable for str-valued ids
}

// ---- _sanitize_tool_pairs ----
Json sanitize_tool_pairs(const Json& msgs) {
    std::set<std::string> surviving;
    for (const auto& msp : msgs.arr) {
        const Json& msg = *msp;
        if (get_str(msg, "role", "") != "assistant") continue;
        const Json* tc = core::json_get(msg, "tool_calls");
        if (!tc || tc->type != Json::Arr) continue;
        for (const auto& csp : tc->arr) {
            std::string cid = get_tool_call_id(*csp);
            if (!cid.empty()) surviving.insert(cid);
        }
    }
    std::set<std::string> result_ids;
    for (const auto& msp : msgs.arr) {
        const Json& msg = *msp;
        if (get_str(msg, "role", "") != "tool") continue;
        const Json* cidp = core::json_get(msg, "tool_call_id");
        if (cidp && cidp->type == Json::Str && !cidp->str.empty()) result_ids.insert(cidp->str);
    }
    std::set<std::string> orphaned;
    for (const std::string& r : result_ids)
        if (!surviving.count(r)) orphaned.insert(r);

    Json cur = core::json_arr();
    for (const auto& msp : msgs.arr) {
        const Json& msg = *msp;
        bool drop = false;
        if (get_str(msg, "role", "") == "tool") {
            const Json* cidp = core::json_get(msg, "tool_call_id");
            if (cidp && cidp->type == Json::Str && orphaned.count(cidp->str)) drop = true;
        }
        if (!drop) core::arr_push(cur, core::json_deep_copy(msg));
    }

    std::set<std::string> missing;
    for (const std::string& s : surviving)
        if (!result_ids.count(s)) missing.insert(s);

    if (!missing.empty()) {
        Json patched = core::json_arr();
        for (const auto& msp : cur.arr) {
            const Json& msg = *msp;
            core::arr_push(patched, core::json_deep_copy(msg));
            if (get_str(msg, "role", "") != "assistant") continue;
            const Json* tc = core::json_get(msg, "tool_calls");
            if (!tc || tc->type != Json::Arr) continue;
            for (const auto& csp : tc->arr) {
                std::string cid = get_tool_call_id(*csp);
                if (missing.count(cid)) {
                    Json stub = core::json_obj();
                    core::obj_set(stub, "role", core::json_str("tool"));
                    core::obj_set(stub, "content",
                        core::json_str("[Result from earlier conversation \u2014 see context summary above]"));
                    core::obj_set(stub, "tool_call_id", core::json_str(cid));
                    core::arr_push(patched, stub);
                }
            }
        }
        return patched;
    }
    return cur;
}

// ---- _align_boundary_forward ----
inline long long align_boundary_forward(const Json& msgs, long long idx) {
    const long long n = (long long)msgs.arr.size();
    while (idx < n && get_str(*msgs.arr[(size_t)idx], "role", "") == "tool") ++idx;
    return idx;
}

// ---- _align_boundary_backward ----
inline long long align_boundary_backward(const Json& msgs, long long idx) {
    const long long n = (long long)msgs.arr.size();
    if (idx <= 0 || idx >= n) return idx;
    long long check = idx - 1;
    while (check >= 0 && get_str(*msgs.arr[(size_t)check], "role", "") == "tool") --check;
    if (check >= 0 && get_str(*msgs.arr[(size_t)check], "role", "") == "assistant" &&
        core::py_truthy(core::json_get(*msgs.arr[(size_t)check], "tool_calls")))
        idx = check;
    return idx;
}

// ---- _find_tail_cut_by_tokens ----
long long find_tail_cut_by_tokens(const Json& msgs, long long head_end, long long token_budget,
                                  long long protect_last_n) {
    const long long n = (long long)msgs.arr.size();
    const long long min_tail = protect_last_n;
    long long accumulated = 0, cut_idx = n;
    for (long long i = n - 1; i >= head_end; --i) {
        const Json& msg = *msgs.arr[(size_t)i];
        std::string content = get_str(msg, "content", "");
        long long mt = (long long)content.size() / _CHARS_PER_TOKEN + 10;
        const Json* tc = core::json_get(msg, "tool_calls");
        if (tc && tc->type == Json::Arr) {
            for (const auto& csp : tc->arr) {
                const Json& call = *csp;
                if (call.type != Json::Obj) continue;
                const Json* fn = core::json_get(call, "function");
                std::string args = (fn && fn->type == Json::Obj) ? get_str(*fn, "arguments", "") : "";
                mt += (long long)args.size() / _CHARS_PER_TOKEN;
            }
        }
        if (accumulated + mt > token_budget && (n - i) >= min_tail) break;
        accumulated += mt;
        cut_idx = i;
    }
    long long fallback_cut = n - min_tail;
    if (cut_idx > fallback_cut) cut_idx = fallback_cut;
    if (cut_idx <= head_end) cut_idx = fallback_cut;
    cut_idx = align_boundary_backward(msgs, cut_idx);
    return std::max(cut_idx, head_end + 1);
}

// ---- compress (summary generator injected) ----
using SummaryFn = std::function<std::string(const Json& turns)>;

Json compress(const Json& msgs, long long current_tokens, const ContextConfig& cfg,
              const SummaryFn& summary_fn) {
    (void)current_tokens;  // feeds logging only; never the return path
    const long long n = (long long)msgs.arr.size();
    if (n <= cfg.protect_first_n + cfg.protect_last_n + 1)
        return core::json_deep_copy(msgs);

    auto pruned = prune_old_tool_results(msgs, cfg.protect_last_n * 3);
    Json messages = std::move(pruned.first);

    long long compress_start = cfg.protect_first_n;
    compress_start = align_boundary_forward(messages, compress_start);
    long long compress_end =
        find_tail_cut_by_tokens(messages, compress_start, cfg.tail_token_budget, cfg.protect_last_n);
    if (compress_start >= compress_end)
        return core::json_deep_copy(messages);

    Json turns = core::json_arr();
    for (long long i = compress_start; i < compress_end; ++i)
        core::arr_push(turns, core::json_deep_copy(*messages.arr[(size_t)i]));
    std::string summary = summary_fn(turns);

    Json compressed = core::json_arr();
    for (long long i = 0; i < compress_start; ++i) {
        Json msg = core::json_deep_copy(*messages.arr[(size_t)i]);
        if (i == 0 && get_str(msg, "role", "") == "system" && cfg.compression_count == 0) {
            std::string c = get_str(msg, "content", "");
            core::obj_set(msg, "content",
                core::json_str(c + "\n\n[Note: Some earlier conversation turns have been "
                               "compacted into a handoff summary to preserve context space. "
                               "The current session state may still reflect earlier work, so "
                               "build on that summary and state rather than re-doing work.]"));
        }
        core::arr_push(compressed, std::move(msg));
    }

    bool merge_into_tail = false;
    if (!summary.empty()) {
        std::string last_head_role =
            (compress_start > 0) ? get_str(*messages.arr[(size_t)(compress_start - 1)], "role", "user") : "user";
        std::string first_tail_role =
            (compress_end < n) ? get_str(*messages.arr[(size_t)compress_end], "role", "user") : "user";
        std::string summary_role =
            (last_head_role == "assistant" || last_head_role == "tool") ? "user" : "assistant";
        if (summary_role == first_tail_role) {
            std::string flipped = (summary_role == "user") ? "assistant" : "user";
            if (flipped != last_head_role) summary_role = flipped;
            else merge_into_tail = true;
        }
        if (!merge_into_tail) {
            Json sm = core::json_obj();
            core::obj_set(sm, "role", core::json_str(summary_role));
            core::obj_set(sm, "content", core::json_str(summary));
            core::arr_push(compressed, std::move(sm));
        }
    }

    for (long long i = compress_end; i < n; ++i) {
        Json msg = core::json_deep_copy(*messages.arr[(size_t)i]);
        if (merge_into_tail && i == compress_end) {
            std::string original = get_str(msg, "content", "");
            core::obj_set(msg, "content", core::json_str(summary + "\n\n" + original));
            merge_into_tail = false;
        }
        core::arr_push(compressed, std::move(msg));
    }

    // self.compression_count += 1 not modeled: single-shot differential, only the
    // ==0 guard is read above.

    return sanitize_tool_pairs(compressed);
}

}  // namespace core
}  // namespace dd
