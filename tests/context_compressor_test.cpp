// tests/context_compressor_test.cpp — differential vs context_compressor.py message-graph core.
// Ground truth from /tmp/gt_cc.py: real ContextCompressor, get_model_context_length forced to 1000
// (threshold_tokens=500, tail_token_budget=100), LLM _generate_summary replaced by a stub,
// estimate_messages_tokens_rough patched (logging-only, never in return path).
// Config for the instance under test: protect_first_n=1, protect_last_n=2.
#include "dd/core/json.hpp"
#include "dd/core/context_compressor.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>

using namespace dd::core;

static int g_fail = 0, g_pass = 0;
static void chk(bool c, const std::string& w) {
    if (c) ++g_pass;
    else { ++g_fail; std::cout << "FAIL: " << w << "\n"; }
}

static std::string read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static Json gt(const std::string& tag) { return json_parse(read_file("/tmp/gt_cc_" + tag + ".json")); }

int main() {
    // ---- constants (referenced so no unused warnings; cross-checked vs gt meta) ----
    Json meta = gt("_meta");
    chk(get_str(meta, "ph", "") == _PRUNED_TOOL_PLACEHOLDER, "const PH");
    chk(get_str(meta, "prefix", "") == SUMMARY_PREFIX, "const SUMMARY_PREFIX");
    chk(get_str(meta, "legacy", "") == LEGACY_SUMMARY_PREFIX, "const LEGACY");
    chk(get_str(meta, "offload_prefix", "") == _OFFLOAD_PREFIX, "const OFFLOAD_PREFIX");
    {
        Json subjs = *json_get(meta, "subj");
        chk(subjs.type == Json::Arr && subjs.arr.size() == 6, "const SUBJ count");
    }

    const long long THRESH = 500, TAIL = 100, LIVE = 12;
    ContextConfig cfg;
    cfg.protect_first_n = 1;
    cfg.protect_last_n = 2;
    cfg.threshold_tokens = THRESH;
    cfg.tail_token_budget = TAIL;
    cfg.live_window_messages = LIVE;
    cfg.compression_count = 0;
    chk((long long)json_get(meta, "threshold_tokens")->num == cfg.threshold_tokens, "cfg threshold");
    chk((long long)json_get(meta, "tail_token_budget")->num == cfg.tail_token_budget, "cfg tail");

    Json in_msgs = gt("in_msgs");
    Json orphan = gt("orphan");
    Json toolish = gt("toolish");

    // ---- payload_chars ----
    chk(payload_chars(in_msgs) == 724, "payload_chars");
    chk(payload_chars(json_arr()) == 0, "payload empty");

    // ---- live_window_chars ----
    chk(live_window_chars(THRESH) == 1000, "live_window_chars 500->1000");
    chk(live_window_chars(0) == 0, "live_window_chars 0->0");

    // ---- prune_floor ----
    {
        Json pf = gt("prunefloor");
        for (long long t : {0LL, 10LL, 299LL, 300LL, 400LL, 10000LL}) {
            const Json* exp = json_get(pf, std::to_string(t));
            chk(prune_floor(t, THRESH) == (long long)exp->num, "prune_floor " + std::to_string(t));
        }
    }

    // ---- adaptive_tail ----
    chk(adaptive_tail(in_msgs, LIVE, live_window_chars(THRESH)) == 9, "adaptive_tail");
    chk(adaptive_tail(in_msgs, LIVE, 0) == std::min<long long>(LIVE, (long long)in_msgs.arr.size()),
        "adaptive_tail budget0");

    // ---- prune_old_tool_results ----
    {
        auto pr = prune_old_tool_results(in_msgs, cfg.protect_last_n * 3);
        Json exp = gt("prune");
        chk(json_dump(pr.first) == json_dump(*json_get(exp, "msgs")), "prune msgs");
        chk(pr.second == (long long)json_get(exp, "count")->num, "prune count");
        auto pe = prune_old_tool_results(json_arr(), 6);
        chk(pe.second == 0 && json_dump(pe.first) == "[]", "prune empty");
    }

    // ---- tool_call_names ----
    chk(json_dump(tool_call_names(in_msgs)) == json_dump(gt("toolcallnames")), "tool_call_names");

    // ---- is_offloaded ----
    {
        Json iso = gt("isoffloaded");
        for (auto& kv : iso.obj) {
            chk(is_offloaded(kv.first) == (kv.second->type == Json::Bool && kv.second->b),
                "is_offloaded [" + kv.first + "]");
        }
    }

    // ---- describe_arguments ----
    {
        Json da_in = json_obj({
            {"empty", std::make_shared<Json>(json_str(""))},
            {"ws", std::make_shared<Json>(json_str("   "))},
            {"nonjson", std::make_shared<Json>(json_str("{not json"))},
            {"nondict", std::make_shared<Json>(json_str("[1,2]"))},
            {"num", std::make_shared<Json>(json_num(5))},
            {"nokeys", std::make_shared<Json>(json_str("{\"foo\": \"bar\", \"num\": 1}"))},
            {"path", std::make_shared<Json>(json_str("{\"path\": \"a   b\\n\\tc\", \"query\": \"z\"}"))},
            {"long", std::make_shared<Json>(json_str("{\"command\": \"" + std::string(200, 'y') + "\"}"))},
        });
        Json de = gt("describe");
        for (auto& kv : da_in.obj) {
            const Json* raw = json_get(da_in, kv.first);
            chk(describe_arguments(raw) == get_str(de, kv.first, ""), "describe [" + kv.first + "]");
        }
    }

    // ---- tool_call_subjects ----
    chk(json_dump(tool_call_subjects(in_msgs)) == json_dump(gt("toolcallsubjects")), "tool_call_subjects");

    // ---- with_summary_prefix ----
    {
        Json wp = gt("withprefix");
        chk(with_summary_prefix("hello") == get_str(wp, "plain", ""), "wp plain");
        chk(with_summary_prefix(LEGACY_SUMMARY_PREFIX + " old") == get_str(wp, "legacy", ""), "wp legacy");
        chk(with_summary_prefix(SUMMARY_PREFIX + "\nbody") == get_str(wp, "cur", ""), "wp cur");
        chk(with_summary_prefix("") == get_str(wp, "empty", ""), "wp empty");
        chk(with_summary_prefix("") == get_str(wp, "none", ""), "wp none(None)");
    }

    // ---- get_tool_call_id ----
    {
        Json tic = gt("toolcallid");
        chk(get_tool_call_id(json_obj({{"id", std::make_shared<Json>(json_str("c9"))}})) == get_str(tic, "d", ""), "tci d");
        chk(get_tool_call_id(json_obj({{"function", std::make_shared<Json>(json_obj())}})) == "", "tci absent");
        chk(get_tool_call_id(json_num(123)) == "", "tci nonstr");
        // "nul" fixture omitted: Python tc.get("id","") returns None (JSON null) for a present-null
        // id; std::string cannot carry it and real message graphs always use str ids.
    }

    // ---- align_boundary_forward ----
    {
        Json af = gt("alignfwd");
        chk(align_boundary_forward(toolish, 1) == (long long)json_get(af, "i1")->num, "alignfwd i1");
        chk(align_boundary_forward(toolish, 3) == (long long)json_get(af, "i3")->num, "alignfwd i3");
        chk(align_boundary_forward(toolish, 0) == (long long)json_get(af, "i0")->num, "alignfwd i0");
    }

    // ---- align_boundary_backward ----
    {
        Json ab = gt("alignbwd");
        for (auto& kv : ab.obj) {
            long long idx = std::stoll(kv.first.substr(1));
            chk(align_boundary_backward(toolish, idx) == (long long)kv.second->num, "alignbwd " + kv.first);
        }
    }

    // ---- find_tail_cut_by_tokens ----
    {
        Json ft = gt("findtail");
        chk(find_tail_cut_by_tokens(in_msgs, 1, cfg.tail_token_budget, cfg.protect_last_n)
                == (long long)json_get(ft, "he1")->num, "findtail he1");
        chk(find_tail_cut_by_tokens(in_msgs, 0, cfg.tail_token_budget, cfg.protect_last_n)
                == (long long)json_get(ft, "he0")->num, "findtail he0");
    }

    // ---- sanitize_tool_pairs ----
    {
        Json san = gt("sanitize");
        chk(json_dump(sanitize_tool_pairs(orphan)) == json_dump(*json_get(san, "msgs")), "sanitize orphan");
        chk(json_dump(sanitize_tool_pairs(in_msgs)) == json_dump(in_msgs), "sanitize clean no-op");
    }

    // ---- compress (summary stubbed) ----
    {
        SummaryFn stub = [](const Json&) { return std::string("STUB_SUMMARY"); };
        chk(json_dump(compress(in_msgs, 999, cfg, stub)) == json_dump(gt("compress")), "compress stub");
    }
    // compress too-few: passthrough (2 msgs <= 1+2+1=4)
    {
        SummaryFn stub = [](const Json&) { return std::string("STUB_SUMMARY"); };
        Json tiny = json_arr({
            std::make_shared<Json>(json_obj({{"role", std::make_shared<Json>(json_str("system"))},
                                            {"content", std::make_shared<Json>(json_str("S"))}})),
            std::make_shared<Json>(json_obj({{"role", std::make_shared<Json>(json_str("user"))},
                                            {"content", std::make_shared<Json>(json_str("U"))}})),
        });
        chk(json_dump(compress(tiny, 0, cfg, stub)) == json_dump(gt("compress_tiny")), "compress tiny");
    }
    // compress: summary unavailable ("" == Python None) -> middle dropped, no summary msg
    {
        SummaryFn none_fn = [](const Json&) { return std::string(""); };
        chk(json_dump(compress(in_msgs, 999, cfg, none_fn)) == json_dump(gt("compress_none")), "compress none");
    }

    std::cout << "context_compressor: " << g_pass << " tests: " << (g_fail ? "FAIL" : "PASS") << "\n";
    return g_fail ? 1 : 0;
}
