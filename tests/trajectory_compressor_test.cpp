// tests/trajectory_compressor_test.cpp — differential vs trajectory_compressor.py message-graph core.
// Ground truth produced by /tmp/gt_emit.py (real class, tokenizer forced to the len//4 fallback,
// summarization LLM replaced by a deterministic stub that bumps summarization_api_calls once).
// The injected token counter here MUST equal the fallback used to generate gt (len//4).
#include "dd/core/json.hpp"
#include "dd/core/trajectory_compressor.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>

using namespace dd::core;

static int g_fail = 0, g_pass = 0;
static void chk(bool c, const std::string& w) { if (c) ++g_pass; else { ++g_fail; std::cout << "FAIL: " << w << "\n"; } }

static std::string read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static std::string rep(const std::string& unit, int n) {
    std::string s; s.reserve(unit.size() * (size_t)n);
    for (int i = 0; i < n; ++i) s += unit;
    return s;
}

// Build a trajectory Json array of {from,value} objects.
static Json mk_traj(std::initializer_list<std::pair<std::string, std::string>> turns) {
    std::vector<JsonSP> v;
    for (auto& kv : turns) {
        Json o = json_obj({{"from", std::make_shared<Json>(json_str(kv.first))},
                           {"value", std::make_shared<Json>(json_str(kv.second))}});
        v.push_back(std::make_shared<Json>(std::move(o)));
    }
    return json_arr(std::move(v));
}

int main() {
    TokFn tok = [](const std::string& t){ return t.empty() ? 0LL : (long long)t.size()/4; };
    auto stub_summary = [](const std::string&, TrajectoryMetrics& m){ ++m.summarization_api_calls; return std::string("[CONTEXT SUMMARY]: SYNTH"); };

    CompressionConfig cfg; cfg.target_max_tokens = 40; cfg.summary_target_tokens = 10;

    auto run_and_cmp = [&](const Json& traj, const std::string& tag) {
        TrajectoryMetrics m{};
        Json out = compress_trajectory(traj, cfg, tok, stub_summary, m);
        Json gt = json_parse(read_file("/tmp/gt_" + tag + ".json"));
        Json gt_m = json_parse(read_file("/tmp/gt_" + tag + "_m.json"));
        chk(json_dump(out) == json_dump(gt), tag + " compressed");
        chk(json_dump(m.to_dict()) == json_dump(gt_m), tag + " metrics");
    };

    Json A = mk_traj({
        {"system", std::string(300,'S')},
        {"human",  std::string(300,'H')},
        {"gpt",    std::string(300,'G')},
        {"tool",   std::string(300,'T')},
        {"gpt",    rep("middle1",400)},
        {"human",  rep("middle2",400)},
        {"gpt",    rep("tailA",200)},
        {"tool",   rep("tailB",200)},
        {"human",  rep("tailC",200)},
        {"gpt",    rep("tailD",200)},
    });
    run_and_cmp(A, "A");
    run_and_cmp(mk_traj({{"system","x"},{"human","y"}}), "B");
    run_and_cmp(mk_traj({{"system",std::string(300,'S')},{"human",std::string(300,'H')},{"gpt",std::string(300,'G')},{"tool",std::string(300,'T')}}), "C");

    // ---- helpers ----
    {
        Json tj = mk_traj({{"system","a"},{"HUMAN", std::string(5000,'b')}});
        chk(extract_turn_content(tj, 0, 2) == read_file("/tmp/gt_ex.txt"), "extract truncation");
        chk(extract_turn_content(tj, 1, 1).empty(), "extract empty-range");
        chk(coerce_summary_content(json_null()) == read_file("/tmp/gt_c1.txt"), "coerce None");
        chk(coerce_summary_content(json_num(0)) == read_file("/tmp/gt_c2.txt"), "coerce 0");
        chk(ensure_summary_prefix("  hello  ") == read_file("/tmp/gt_e1.txt"), "ensure prefix trim");
        chk(ensure_summary_prefix("") == read_file("/tmp/gt_e2.txt"), "ensure prefix empty");
        chk(ensure_summary_prefix("[CONTEXT SUMMARY]: hi") == "[CONTEXT SUMMARY]: hi", "ensure prefix already");
    }

    std::cout << "trajectory_compressor: " << g_pass << " tests: " << (g_fail ? "FAIL" : "PASS") << "\n";
    return g_fail ? 1 : 0;
}
