// tests/model_metadata_test.cpp — differential vs agent/model_metadata.py (extraction island).
// Ground truth captured by running the real module via /tmp/gt_mm.py.
#include "dd/core/json.hpp"
#include "dd/core/model_metadata.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <optional>

using namespace dd::core;

static int g_fail = 0, g_pass = 0;
static void chk(bool cond, const std::string& what) {
    if (cond) { ++g_pass; } else { ++g_fail; std::cout << "FAIL: " << what << "\n"; }
}

static std::string optstr(std::optional<long long> o) { return o ? "Some(" + std::to_string(*o) + ")" : "None"; }

int main() {
    // ---- coerce_reasonable_int (mirrors _coerce_reasonable_int) ----
    {
        auto t = [](const Json& v){ return coerce_reasonable_int(v); };
        chk(!t(json_bool(true)),  "coerce True->None");
        chk(!t(json_bool(false)), "coerce False->None");
        chk(*t(json_num(32000)) == 32000, "coerce 32000->32000");
        chk(*t(json_str("1,000,000")) == 1000000, "coerce '1,000,000'->1000000");
        chk(*t(json_str("32000")) == 32000, "coerce '32000'->32000");
        chk(*t(json_str(" 32000 ")) == 32000, "coerce ' 32000 '->32000");
        chk(!t(json_str("abc")), "coerce 'abc'->None");
        chk(!t(json_num(3.5)),  "coerce 3.5->None (trunc<min)");
        chk(!t(json_num(10000000000.0)), "coerce 1e10->None (>max)");
        chk(!t(json_num(1023)),  "coerce 1023->None (<min)");
        chk(*t(json_num(1024)) == 1024, "coerce 1024->1024");
        chk(!t(json_null()), "coerce None->None");
        chk(!t(json_arr()), "coerce [1]->None (arr)");
        chk(!t(json_str("5")), "coerce '5'->None (<min)");
        chk(!t(json_str("0x10")), "coerce '0x10'->None (hex rejected)");
        chk(!t(json_str("5.5")), "coerce '5.5'->None (float-str rejected by int())");
    }

    // ---- iter_nested_dicts pre-order key-lists ----
    {
        Json d = json_parse("{\"a\":1,\"b\":{\"c\":2},\"d\":[{\"e\":3},5,{\"g\":{\"h\":4}}]}");
        auto nodes = iter_nested_dicts_root(d);
        std::vector<std::vector<std::string>> got;
        for (const Json* m : nodes) { std::vector<std::string> ks; for (const auto& kv : m->obj) ks.push_back(kv.first); got.push_back(ks); }
        std::vector<std::vector<std::string>> want = {{ "a","b","d" }, { "c" }, { "e" }, { "g" }, { "h" }};
        chk(got == want, "iter_nested_dicts pre-order keyseq");
    }

    // ---- extract_first_int / context / completion ----
    {
        Json p = json_parse("{\"data\":{\"max_context_length\":\"128,000\"},\"model\":true,\"x\":{\"max_tokens\":\"900\"}}");
        chk(optstr(extract_first_int(p, {"max_context_length"})) == "Some(128000)", "first_int max_context_length");
        chk(optstr(extract_context_length(p)) == "Some(128000)", "extract_context_length");
        chk(optstr(extract_max_completion_tokens(p)) == "None", "extract_max_completion_tokens (900<min)");
    }

    // ---- extract_pricing ----
    {
        Json q = json_parse("{\"pricing\":{\"input_cost_per_token\":0.01,\"output_cost_per_token\":0.03,\"junk\":null}}");
        Json got = extract_pricing(q);
        chk(json_dump(got) == json_dump(json_parse("{\"prompt\":0.01,\"completion\":0.03}")), "pricing alias map");
        chk(json_dump(extract_pricing(json_parse("{\"x\":1}"))) == "{}", "pricing none->{}");
        Json q2 = json_parse("{\"prompt\":0.5,\"cache_read\":0.0}");
        chk(json_dump(extract_pricing(q2)) == json_dump(json_parse("{\"prompt\":0.5,\"cache_read\":0.0}")), "pricing keeps 0.0");
        chk(json_dump(extract_pricing(json_parse("{\"prompt\":\"\"}"))) == "{}", "pricing empty-str excluded");
    }

    // ---- py_str lengths (feed estimate) ----
    {
        struct { const char* js; const char* want; } cases[] = {
            {"{\"role\":\"user\",\"content\":\"hi\"}", "{'role': 'user', 'content': 'hi'}"},
            {"{}", "{}"},
            {"{\"n\":3}", "{'n': 3}"},
            {"{\"f\":0.5}", "{'f': 0.5}"},
            {"{\"t\":true}", "{'t': True}"},
            {"{\"e\":null}", "{'e': None}"},
            {"{\"s\":[{\"k\":1}]}", "{'s': [{'k': 1}]}"},
        };
        for (auto& c : cases) {
            Json v = json_parse(c.js);
            chk(py_str(v) == c.want, std::string("py_str ")+c.js);
        }
    }

    // ---- rough estimators ----
    {
        chk(estimate_tokens_rough("abcd") == 1, "rough abcd=1");
        chk(estimate_tokens_rough("") == 0, "rough empty=0");
        Json ms = json_parse("[{\"role\":\"user\",\"content\":\"hello there\"}]");
        chk(estimate_messages_tokens_rough(ms) == 10, "msgs rough=10");
        chk(estimate_messages_tokens_rough(json_arr()) == 0, "msgs empty=0");
        Json tools = json_parse("[{\"type\":\"function\",\"function\":{\"name\":\"x\"}}]");
        chk(estimate_request_tokens_rough(ms, "sys prompt text", tools) == 26, "req rough=26");
        chk(estimate_request_tokens_rough(ms, "", json_null()) == 10, "req no-tools no-sys=10");
    }

    std::cout << "model_metadata: " << g_pass << " tests: " << (g_fail ? "FAIL" : "PASS") << "\n";
    return g_fail ? 1 : 0;
}
