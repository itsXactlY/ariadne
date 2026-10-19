// Differential test for agent/kv_watermarks.py pure subset vs Python truth.
#include "dd/agent/kv_watermarks.hpp"
#include <optional>
#include <string>
#include <vector>
#include <cmath>
#include "dd/core/json.hpp"
#include <iostream>

using namespace dd;
using namespace dd::agent;
using namespace dd::core;

static Json mkobj(std::initializer_list<std::pair<std::string, Json>> ms) {
    std::vector<std::pair<std::string, JsonSP>> w;
    for (auto& m : ms) { auto sp = std::make_shared<Json>(std::move(m.second)); w.push_back({m.first, sp}); }
    return json_obj(w);
}
static Json mkarr(std::initializer_list<Json> vs) {
    std::vector<JsonSP> x; for (auto& v : vs) x.push_back(std::make_shared<Json>(std::move(v))); return json_arr(x);
}

static int g_fail = 0;
static void chk(const std::string& what, bool ok) {
    if (!ok) { std::cout << "FAIL " << what << "\n"; ++g_fail; }
}

int main() {
    // ---- level boundaries (defaults 0.6/0.8/0.9) ----
    struct L { int fast, pool; std::string exp; } Ls[] = {
        {100,0,"ok"},{100,60,"low"},{100,59,"ok"},{100,80,"high"},{100,79,"low"},
        {100,90,"hard"},{100,89,"high"},{100,99,"hard"},{100,1000,"hard"},
        {0,5,"ok"},{-5,5,"ok"},{100,100,"hard"},
    };
    for (auto& l : Ls) {
        Watermarks w; w.fast_layer_tokens = l.fast;
        chk("level:" + std::to_string(l.fast) + ":" + std::to_string(l.pool), w.level(l.pool) == l.exp);
    }
    // ---- hard_tokens truncation ----
    chk("hard:100=90", ([](){Watermarks w;w.fast_layer_tokens=100;return w.hard_tokens();}())==90);
    chk("hard:1000=900", ([](){Watermarks w;w.fast_layer_tokens=1000;return w.hard_tokens();}())==900);
    chk("hard:7=6", ([](){Watermarks w;w.fast_layer_tokens=7;return w.hard_tokens();}())==6);
    chk("hard:0=0", ([](){Watermarks w;w.fast_layer_tokens=0;return w.hard_tokens();}())==0);
    chk("hard:3=2", ([](){Watermarks w;w.fast_layer_tokens=3;return w.hard_tokens();}())==2);
    // ---- high_ratio_of_hard ----
    { Watermarks w; chk("ratio:def=0.888888888888889", std::fabs(w.high_ratio_of_hard()-0.888888888888889)<1e-12); }
    { Watermarks w; w.hard=0.0;  chk("ratio:hard0=0.0", w.high_ratio_of_hard()==0.0); }
    { Watermarks w; w.hard=0.5;  chk("ratio:0.5=1.6", std::fabs(w.high_ratio_of_hard()-1.6)<1e-12); }
    // ---- server_root ----
    auto ck = [](const std::string& u, const std::optional<std::string>& exp){
        auto got = server_root(u);
        chk("root:"+u, got == exp);
    };
    ck("http://127.0.0.1:8080/v1", std::optional<std::string>("http://127.0.0.1:8080"));
    ck("http://localhost:8080", std::optional<std::string>("http://localhost:8080"));
    ck("https://api.remote.com/v1", std::nullopt);
    ck("127.0.0.1/", std::optional<std::string>("127.0.0.1"));
    ck("http://LOCALHOST:8080/v1/", std::optional<std::string>("http://LOCALHOST:8080"));
    ck("", std::nullopt);
    ck("/v1", std::nullopt);
    ck("http://127.0.0.1:8080/v1/", std::optional<std::string>("http://127.0.0.1:8080"));
    // ---- slots ----
    Json slots = mkarr({
        mkobj({{"id",json_num(0)},{"n_prompt_tokens",json_num(42)},{"is_processing",json_bool(true)}}),
        mkobj({{"id",json_num(1)},{"is_processing",json_bool(false)}}),
    });
    chk("slot:0 tok=42", slot_tokens(slots,0)==42);
    chk("slot:0 busy=T", slot_busy(slots,0)==true);
    chk("slot:1 tok=0", slot_tokens(slots,1)==0);
    chk("slot:1 busy=F", slot_busy(slots,1)==false);
    chk("slot:2 tok=0", slot_tokens(slots,2)==0);
    chk("slot:2 busy=F", slot_busy(slots,2)==false);
    Json slots2 = mkarr({
        mkobj({{"id",json_num(3)},{"n_prompt_tokens",json_num(0)}}),
        mkobj({{"id",json_num(4)},{"n_prompt_tokens",json_null()},{"is_processing",json_num(1)}}),
        mkobj({}),
    });
    chk("slot2:3 tok=0", slot_tokens(slots2,3)==0);
    chk("slot2:3 busy=F", slot_busy(slots2,3)==false);
    chk("slot2:4 tok=0", slot_tokens(slots2,4)==0);
    chk("slot2:4 busy=T", slot_busy(slots2,4)==true);
    chk("slot2:5 tok=0", slot_tokens(slots2,5)==0);
    chk("slot2:5 busy=F", slot_busy(slots2,5)==false);

    if (g_fail) { std::cout << "kv_watermarks: FAIL " << g_fail << "\n"; return 1; }
    std::cout << "all kv_watermarks tests passed\n";
    return 0;
}
