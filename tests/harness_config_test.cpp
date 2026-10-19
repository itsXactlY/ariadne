// tests/harness_config_test.cpp — differential harness_config vs Python ground truth.
// Ground truth captured by running agent/harness_config.py HarnessConfig.from_dict.
#include "dd/core/json.hpp"
#include "dd/core/harness_config.hpp"
#include <iostream>
#include <string>
#include <vector>

using namespace dd::core;

static int g_fail = 0, g_pass = 0;
static void chk(bool cond, const std::string& what) {
    if (cond) { ++g_pass; } else { ++g_fail; std::cout << "FAIL: " << what << "\n"; }
}

#define DASH "\xE2\x80\x94"  // em-dash U+2014

static std::vector<std::string> strs(const ProblemVec& pv) {
    std::vector<std::string> v;
    for (const auto& p : pv) v.push_back(p.to_str());
    return v;
}

static void expect(const ProblemVec& pv, const std::vector<std::string>& want, const std::string& tag) {
    auto got = strs(pv);
    chk(got == want, tag);
    if (got != want) {
        std::cout << "  [" << tag << "]\n";
        for (size_t i = 0; i < got.size(); ++i)  std::cout << "   got " << got[i] << "\n";
        for (size_t i = 0; i < want.size(); ++i) std::cout << "   want " << want[i] << "\n";
    }
}

int main() {
    // 1) {} -> 0 problems
    { ProblemVec p; from_dict(json_parse("{}"), p, false); expect(p, {}, "empty"); }

    // 2) compression cluster (type/value/unknown order)
    {
        Json raw = json_parse(
            "{\"compression\":{\"engine\":\"weird\",\"threshold\":\"abc\","
            "\"watermarks\":{\"low\":0.9,\"high\":0.5,\"hard\":0.4},\"bogus\":1}}");
        ProblemVec p;
        from_dict(raw, p, false);
        std::vector<std::string> want = {
            "[type] compression.threshold: expected float, got str ('abc') " DASH " using the default",
            "[value] compression.watermarks: must satisfy 0 < low <= high <= hard <= 1, got 0.9/0.5/0.4",
            "[unknown] compression.bogus: nothing in the harness reads this",
            "[value] compression.engine: 'weird' not in ('retrieval', 'compressor')",
        };
        expect(p, want, "compression");
        // strict: type+value problems joined by "; " (unknown skipped)
        bool threw = false; std::string msg;
        try { ProblemVec ps; from_dict(raw, ps, true); } catch (const std::runtime_error& e) { threw = true; msg = e.what(); }
        chk(threw, "compression strict throws");
        chk(msg == want[0] + "; " + want[1] + "; " + want[3], "compression strict msg");
    }

    // 3) memory cluster
    {
        Json raw = json_parse("{\"memory\":{\"prefetch_timeout_s\":5,\"provider\":\"bogus\"}}");
        ProblemVec p;
        from_dict(raw, p, false);
        std::vector<std::string> want = {
            "[value] memory.provider: 'bogus' not in ('mazemaker', 'mcp', 'builtin', '')",
            "[value] memory.prefetch_timeout_s: 5s is under pod recall latency (5-19s under GPU contention); the join times out and the turn gets no memory context",
        };
        expect(p, want, "memory");
    }

    // 4) mcp_servers cluster
    {
        Json raw = json_parse("{\"mcp_servers\":{\"foo\":{\"enabled\":true,\"transport\":\"http\"}}}");
        ProblemVec p;
        from_dict(raw, p, false);
        expect(p, {"[value] mcp_servers.foo.url: http transport with no url"}, "mcp");
    }

    // 5) agent cluster
    {
        Json raw = json_parse("{\"agent\":{\"gateway_timeout_warning\":900,\"gateway_timeout\":300,\"tool_use_enforcement\":\"x\"}}");
        ProblemVec p;
        from_dict(raw, p, false);
        std::vector<std::string> want = {
            "[value] agent.tool_use_enforcement: 'x' not in ('auto', 'required', 'off')",
            "[value] agent.gateway_timeout_warning: 900s >= gateway_timeout 300s " DASH " the warning can never fire",
        };
        expect(p, want, "agent");
    }

    // 6) dead root keys (model passthrough, compression valid)
    {
        Json raw = json_parse("{\"model\":\"gpt-4\",\"weird_root\":\"x\",\"unknown_x\":1,\"compression\":{\"engine\":\"compressor\"}}");
        ProblemVec p;
        from_dict(raw, p, false);
        expect(p, {
            "[dead] weird_root: no code path reads this section",
            "[dead] unknown_x: no code path reads this section",
        }, "dead");
    }

    // 7) auxiliary with a valid aux task -> 0 problems
    {
        Json raw = json_parse("{\"auxiliary\":{\"foo\":{\"model\":\"m\"}}}");
        ProblemVec p;
        from_dict(raw, p, false);
        expect(p, {}, "auxiliary");
    }

    // 8) mcp_disabled_tools ordering (referenced here; also -Werror unused-function guard)
    {
        Json tools = json_parse("{\"zeta\":{\"enabled\":false},\"alpha\":{\"enabled\":false},\"beta\":{\"enabled\":true}}");
        auto names = mcp_disabled_tools(tools);
        chk((names == std::vector<std::string>{"alpha", "zeta"}), "mcp_disabled_tools sorted");
    }

    std::cout << "harness_config: " << g_pass << " tests: " << (g_fail ? "FAIL" : "PASS") << "\n";
    return g_fail ? 1 : 0;
}
