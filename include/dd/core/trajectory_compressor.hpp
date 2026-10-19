#pragma once
// Port of trajectory_compressor.py message-graph core: CompressionConfig (compression-relevant
// subset), TrajectoryMetrics (+to_dict), _find_protected_indices, _extract_turn_content_for_summary,
// _coerce_summary_content, _ensure_summary_prefix, count_{trajectory,turn}_tokens, compress_trajectory.
// The tokenizer and summarization LLM are OUT of scope (external): the token counter and summary
// generator are injected callables (count_tokens default == the len//4 fallback the real code uses
// when the HF tokenizer fails to load).
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <string>
#include "dd/core/json.hpp"

namespace dd {
namespace core {

using core::Json;

// ---- CompressionConfig (subset relevant to the message-graph algorithm) ----
struct CompressionConfig {
    long long target_max_tokens = 15250;
    long long summary_target_tokens = 750;
    bool protect_first_system = true;
    bool protect_first_human = true;
    bool protect_first_gpt = true;
    bool protect_first_tool = true;
    long long protect_last_n_turns = 4;
    bool add_summary_notice = true;
    std::string summary_notice_text =
        "\n\nSome of your previous tool responses may be summarized to preserve context.";
};

// ---- TrajectoryMetrics (all scalar fields; to_dict renders the Python dict) ----
struct TrajectoryMetrics {
    long long original_tokens = 0;
    long long compressed_tokens = 0;
    long long tokens_saved = 0;
    double compression_ratio = 1.0;

    long long original_turns = 0;
    long long compressed_turns = 0;
    long long turns_removed = 0;

    long long turns_compressed_start_idx = -1;
    long long turns_compressed_end_idx = -1;
    long long turns_in_compressed_region = 0;

    bool was_compressed = false;
    bool still_over_limit = false;
    bool skipped_under_target = false;

    long long summarization_api_calls = 0;
    long long summarization_errors = 0;

    Json to_dict() const {
        double r4 = std::round(compression_ratio * 10000.0) / 10000.0;
        Json region = json_obj({
            {"start_idx", std::make_shared<Json>(json_num((double)turns_compressed_start_idx))},
            {"end_idx", std::make_shared<Json>(json_num((double)turns_compressed_end_idx))},
            {"turns_count", std::make_shared<Json>(json_num((double)turns_in_compressed_region))},
        });
        return json_obj({
            {"original_tokens", std::make_shared<Json>(json_num((double)original_tokens))},
            {"compressed_tokens", std::make_shared<Json>(json_num((double)compressed_tokens))},
            {"tokens_saved", std::make_shared<Json>(json_num((double)tokens_saved))},
            {"compression_ratio", std::make_shared<Json>(json_num(r4))},
            {"original_turns", std::make_shared<Json>(json_num((double)original_turns))},
            {"compressed_turns", std::make_shared<Json>(json_num((double)compressed_turns))},
            {"turns_removed", std::make_shared<Json>(json_num((double)turns_removed))},
            {"compression_region", std::make_shared<Json>(region)},
            {"was_compressed", std::make_shared<Json>(json_bool(was_compressed))},
            {"still_over_limit", std::make_shared<Json>(json_bool(still_over_limit))},
            {"skipped_under_target", std::make_shared<Json>(json_bool(skipped_under_target))},
            {"summarization_api_calls", std::make_shared<Json>(json_num((double)summarization_api_calls))},
            {"summarization_errors", std::make_shared<Json>(json_num((double)summarization_errors))},
        });
    }
};
// Python str.strip() over ASCII whitespace (matches str.strip() default for str content).
static std::string strip_ascii(const std::string& s) {
    size_t a = 0, b = s.size();
    auto ws = [](char c){ return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v'; };
    while (a < b && ws(s[a])) ++a;
    while (b > a && ws(s[b-1])) --b;
    return s.substr(a, b - a);
}

// Python str() of a Json value (repr for containers, no quotes for top-level strings/numbers).
static std::string json_py_str(const Json& v) {
    switch (v.type) {
        case Json::Null: return "None";
        case Json::Bool: return v.b ? "True" : "False";
        case Json::Str:  return v.str;
        case Json::Num: {
            std::string s = json_dump(v);   // compact numeric repr
            return s;
        }
        default: return json_dump(v);  // containers: str()-of-nonstr path unreachable for str-valued output
    }
    return "";
}

// Turn accessors mirroring turn.get(key, default) for string-valued turn dicts.
static std::string turn_get(const Json& turn, const std::string& key, const std::string& def) {
    const Json* v = json_get(turn, key);
    if (!v || v->type != Json::Str) return def;
    return v->str;
}

// ---- count_tokens fallback (== the len//4 path the real code uses w/o a tokenizer) ----
inline long long count_tokens_default(const std::string& text) {
    if (text.empty()) return 0;
    return (long long)text.size() / 4;
}

// ---- count_turn_tokens / count_trajectory_tokens (token counter injected) ----
using TokFn = std::function<long long(const std::string&)>;

std::vector<long long> count_turn_tokens(const Json& traj, const TokFn& tok) {
    std::vector<long long> out;
    for (const auto& sp : traj.arr) out.push_back(tok(turn_get(*sp, "value", "")));
    return out;
}

long long count_trajectory_tokens(const Json& traj, const TokFn& tok) {
    long long s = 0;
    for (const auto& sp : traj.arr) s += tok(turn_get(*sp, "value", ""));
    return s;
}

// ---- _find_protected_indices ----
// Returns (prot_set, compressible_start, compressible_end).
std::tuple<std::set<long long>, long long, long long>
find_protected_indices(const Json& traj, const CompressionConfig& cfg) {
    const long long n = (long long)traj.arr.size();
    std::set<long long> prot_set;

    long long first_system = -1, first_human = -1, first_gpt = -1, first_tool = -1;
    for (long long i = 0; i < n; ++i) {
        std::string role = turn_get(*traj.arr[i], "from", "");
        if (role == "system" && first_system < 0) first_system = i;
        else if (role == "human" && first_human < 0) first_human = i;
        else if (role == "gpt" && first_gpt < 0) first_gpt = i;
        else if (role == "tool" && first_tool < 0) first_tool = i;
    }
    if (cfg.protect_first_system && first_system >= 0) prot_set.insert(first_system);
    if (cfg.protect_first_human && first_human >= 0) prot_set.insert(first_human);
    if (cfg.protect_first_gpt && first_gpt >= 0) prot_set.insert(first_gpt);
    if (cfg.protect_first_tool && first_tool >= 0) prot_set.insert(first_tool);

    for (long long i = std::max(0LL, n - cfg.protect_last_n_turns); i < n; ++i) prot_set.insert(i);

    const long long half = n / 2;  // Python n // 2
    long long head_max = -1, tail_min = n;  // n = empty-tail sentinel; min() over the tail set wins
    bool have_head = false, have_tail = false;
    for (long long idx : prot_set) {
        if (idx < half) { head_max = std::max(head_max, idx); have_head = true; }
        else { tail_min = std::min(tail_min, idx); have_tail = true; }
    }
    long long compressible_start = have_head ? head_max + 1 : 0;
    long long compressible_end = have_tail ? tail_min : n;
    return {prot_set, compressible_start, compressible_end};
}

// ---- _extract_turn_content_for_summary ----
std::string extract_turn_content(const Json& traj, long long start, long long end) {
    std::vector<std::string> parts;
    const long long n = (long long)traj.arr.size();
    long long s = std::max(0LL, start), e = std::min(n, end);
    for (long long i = s; i < e; ++i) {
        std::string role = turn_get(*traj.arr[i], "from", "unknown");
        std::string value = turn_get(*traj.arr[i], "value", "");
        if ((long long)value.size() > 3000) {
            value = value.substr(0, 1500) + "\n...[truncated]...\n" + value.substr(value.size() - 500);
        }
        std::string up = role;
        for (auto& c : up) c = (char)std::toupper((unsigned char)c);
        parts.push_back("[Turn " + std::to_string(i) + " - " + up + "]:\n" + value);
    }
    std::string joined;
    for (size_t k = 0; k < parts.size(); ++k) { if (k) joined += "\n\n"; joined += parts[k]; }
    return joined;
}

// ---- _coerce_summary_content (Any -> safe str): non-str falsy -> ""; non-str truthy -> str(); str -> itself ----
std::string coerce_summary_content(const Json& content) {
    if (content.type == Json::Str) return strip_ascii(content.str);
    bool falsy = (content.type == Json::Null) ||
                 (content.type == Json::Bool && !content.b) ||
                 (content.type == Json::Str && content.str.empty()) ||
                 (content.type == Json::Arr && content.arr.empty()) ||
                 (content.type == Json::Obj && content.obj.empty()) ||
                 (content.type == Json::Num && content.num == 0.0);
    std::string s = falsy ? std::string() : json_py_str(content);
    return strip_ascii(s);
}

// ---- _ensure_summary_prefix ----
std::string ensure_summary_prefix(const std::string& summary) {
    std::string text = strip_ascii(summary);
    if (text.rfind("[CONTEXT SUMMARY]:", 0) == 0) return text;
    if (text.empty()) return "[CONTEXT SUMMARY]:";
    return "[CONTEXT SUMMARY]: " + text;
}

// ---- compress_trajectory (token counter + summary generator injected) ----
// summary_fn(content, metrics&) mirrors _generate_summary: it may bump metrics.summarization_api_calls.
Json compress_trajectory(
    const Json& traj,
    const CompressionConfig& cfg,
    const TokFn& tok,
    const std::function<std::string(const std::string&, TrajectoryMetrics&)>& summary_fn,
    TrajectoryMetrics& metrics) {
    const long long n = (long long)traj.arr.size();
    metrics.original_turns = n;
    std::vector<long long> tt = count_turn_tokens(traj, tok);
    long long total = 0;
    for (long long v : tt) total += v;
    metrics.original_tokens = total;

    if (total <= cfg.target_max_tokens) {
        metrics.skipped_under_target = true;
        metrics.compressed_tokens = total;
        metrics.compressed_turns = n;
        metrics.compression_ratio = 1.0;
        return json_deep_copy(traj);
    }

    auto [prot_set, cstart, cend] = find_protected_indices(traj, cfg);
    if (cstart >= cend) {
        metrics.compressed_tokens = total;
        metrics.compressed_turns = n;
        metrics.still_over_limit = total > cfg.target_max_tokens;
        return json_deep_copy(traj);
    }

    long long tokens_to_save = total - cfg.target_max_tokens;
    long long target_tokens_to_compress = tokens_to_save + cfg.summary_target_tokens;

    long long accumulated = 0, compress_until = cstart;
    for (long long i = cstart; i < cend; ++i) {
        accumulated += tt[i];
        compress_until = i + 1;
        if (accumulated >= target_tokens_to_compress) break;
    }
    if (accumulated < target_tokens_to_compress && compress_until < cend) {
        compress_until = cend;
        long long acc2 = 0;
        for (long long i = cstart; i < cend; ++i) acc2 += tt[i];
        accumulated = acc2;
    }

    metrics.turns_compressed_start_idx = cstart;
    metrics.turns_compressed_end_idx = compress_until;
    metrics.turns_in_compressed_region = compress_until - cstart;

    std::string content = extract_turn_content(traj, cstart, compress_until);
    std::string summary = summary_fn(content, metrics);

    std::vector<JsonSP> compressed;
    for (long long i = 0; i < cstart; ++i) {
        Json turn = json_deep_copy(*traj.arr[i]);
        if (turn_get(turn, "from", "") == "system" && cfg.add_summary_notice) {
            obj_set(turn, "value", json_str(turn_get(turn, "value", "") + cfg.summary_notice_text));
        }
        compressed.push_back(std::make_shared<Json>(std::move(turn)));
    }
    {
        Json summary_turn = json_obj({{"from", std::make_shared<Json>(json_str("human"))},
                                      {"value", std::make_shared<Json>(json_str(summary))}});
        compressed.push_back(std::make_shared<Json>(std::move(summary_turn)));
    }
    for (long long i = compress_until; i < n; ++i)
        compressed.push_back(std::make_shared<Json>(json_deep_copy(*traj.arr[i])));

    Json out = json_arr(std::move(compressed));

    metrics.compressed_turns = (long long)out.arr.size();
    metrics.compressed_tokens = count_trajectory_tokens(out, tok);
    metrics.turns_removed = metrics.original_turns - metrics.compressed_turns;
    metrics.tokens_saved = metrics.original_tokens - metrics.compressed_tokens;
    metrics.compression_ratio = (double)metrics.compressed_tokens / (double)std::max(1LL, metrics.original_tokens);
    metrics.was_compressed = true;
    metrics.still_over_limit = metrics.compressed_tokens > cfg.target_max_tokens;
    return out;
}

}  // namespace core
}  // namespace dd
