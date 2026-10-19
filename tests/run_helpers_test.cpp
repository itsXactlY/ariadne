// Differential test for run_agent.py pure helpers vs captured Python truth.
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "dd/core/json.hpp"
#include "dd/agent/run_helpers.hpp"

using namespace dd;
using namespace dd::agent;

static int failures = 0;
static void chk(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::cout << "FAIL " << what << "\n";
    }
}
static void chk_near(double got, double want, const std::string& what) {
    chk(std::fabs(got - want) < 1e-9, what);
}

static Json mkobj(std::initializer_list<std::pair<std::string, Json>> kv) {
    Json o = json_obj();
    for (const auto& p : kv) obj_set(o, p.first, p.second);
    return o;
}
static Json mkarr(std::initializer_list<Json> v) {
    Json a = json_arr();
    for (const auto& x : v) arr_push(a, x);
    return a;
}

static void expect_hot(Json cfg, double want, const std::string& tag) {
    chk_near(hot_swap_hard_ratio_from(cfg), want, "hot:" + tag);
}
static void expect_tb(Json cfg, long long want, const std::string& tag) {
    chk(thinking_budget_from(cfg) == want, "tb:" + tag);
}

int main() {
    // ---- existing helpers still behave ----
    chk(_is_destructive_command("rm -rf /"), "destr:rm");
    chk(!_is_destructive_command("ls -la"), "destr:ls");
    chk(_paths_overlap({"/", "a", "b"}, {"/", "a"}), "overlap:prefix");
    chk(!_paths_overlap({"/", "a", "b"}, {"/", "a", "c"}), "overlap:diverge");

    // ---- hot_swap_hard_ratio_from ----
    expect_hot(json_obj(), 0.85, "empty");
    expect_hot(mkobj({{"hot_swap_hard_ratio", json_num(0.7)}}), 0.7, "num0.7");
    expect_hot(mkobj({{"hot_swap_hard_ratio", json_num(0.3)}}), 0.85, "num0.3");
    expect_hot(mkobj({{"hot_swap_hard_ratio", json_num(0.99)}}), 0.99, "num0.99");
    expect_hot(mkobj({{"hot_swap_hard_ratio", json_num(1.5)}}), 0.85, "num1.5");
    expect_hot(mkobj({{"hot_swap_hard_ratio", json_num(0.5)}}), 0.85, "num0.5-exclusive");
    expect_hot(mkobj({{"hot_swap_hard_ratio", json_str("0.8")}}), 0.8, "str0.8");
    expect_hot(mkobj({{"hot_swap_hard_ratio", json_str("0.75")}}), 0.75, "str0.75");
    expect_hot(mkobj({{"hot_swap_hard_ratio", json_str(" 0.85 ")}}), 0.85, "str-padded");
    expect_hot(mkobj({{"hot_swap_hard_ratio", json_str("1e-1")}}), 0.85, "str1e-1-range");
    expect_hot(mkobj({{"hot_swap_hard_ratio", json_str("abc")}}), 0.85, "str-abc");
    expect_hot(mkobj({{"hot_swap_hard_ratio", json_str("1.2.3")}}), 0.85, "str-1.2.3");
    expect_hot(mkobj({{"hot_swap_hard_ratio", json_str("1e")}}), 0.85, "str-1e");
    expect_hot(mkobj({{"hot_swap_hard_ratio", json_null()}}), 0.85, "null");
    expect_hot(mkobj({{"hot_swap_hard_ratio", json_bool(true)}}), 0.85, "true->1.0");
    expect_hot(mkobj({{"hot_swap_hard_ratio", json_bool(false)}}), 0.85, "false->0");
    expect_hot(mkobj({{"hot_swap_hard_ratio", mkarr({json_num(1)})}}), 0.85, "arr");

    // ---- thinking_budget_from ----
    expect_tb(json_obj(), 1024, "absent");
    expect_tb(mkobj({{"thinking_budget", json_num(500)}}), 500, "num500");
    expect_tb(mkobj({{"thinking_budget", json_num(0)}}), 0, "num0");
    expect_tb(mkobj({{"thinking_budget", json_num(-5)}}), 0, "numneg-clamp");
    expect_tb(mkobj({{"thinking_budget", json_num(3.9)}}), 3, "num-trunc");
    expect_tb(mkobj({{"thinking_budget", json_str("42")}}), 42, "str42");
    expect_tb(mkobj({{"thinking_budget", json_str(" 7 ")}}), 7, "str-padded");
    expect_tb(mkobj({{"thinking_budget", json_str("1_0")}}), 10, "str-underscore");
    expect_tb(mkobj({{"thinking_budget", json_str("0x10")}}), 1024, "str-hex-reject");
    expect_tb(mkobj({{"thinking_budget", json_str("3.5")}}), 1024, "str-float-reject");
    expect_tb(mkobj({{"thinking_budget", json_null()}}), 1024, "null");
    expect_tb(mkobj({{"thinking_budget", json_bool(true)}}), 1, "true->1");
    expect_tb(mkobj({{"thinking_budget", mkarr({json_num(1)})}}), 1024, "arr");


    // ---- _coerce_output_history_limit ----
    chk(_coerce_output_history_limit(json_num(100)) == 100, "co:100");
    chk(_coerce_output_history_limit(json_num(5)) == 10, "co:5-clamp");
    chk(_coerce_output_history_limit(json_null()) == 200, "co:null");
    chk(_coerce_output_history_limit(json_str("3.5")) == 200, "co:float-reject");
    chk(_coerce_output_history_limit(json_str("42")) == 42, "co:str42");
    chk(_coerce_output_history_limit(json_bool(true)) == 10, "co:bool-true");
    chk(_coerce_output_history_limit(json_num(3.9)) == 10, "co:float-trunc");
    chk(_coerce_output_history_limit(mkarr({json_num(1)})) == 200, "co:arr");
    chk(_coerce_output_history_limit(json_str(" 7 ")) == 10, "co:padded");
    chk(_coerce_output_history_limit(json_str("0")) == 10, "co:zero");
    chk(_coerce_output_history_limit(json_num(-5)) == 10, "co:neg-clamp");
    chk(_coerce_output_history_limit(json_str("1_0")) == 10, "co:underscore");
    chk(_coerce_output_history_limit(json_str("0x10")) == 200, "co:hex-reject");
    if (failures == 0) {
        std::cout << "all run-helper tests passed\n";
        return 0;
    }
    std::cout << failures << " run-helper tests FAILED\n";
    return 1;
}
