// Differential test for dd/agent/task_intensity.hpp vs Python ground truth.
#include "dd/agent/task_intensity.hpp"
#include <cstdio>
#include <string>
#include <vector>

using namespace dd::agent;

static int failures = 0;
static int checks = 0;
#define CHK(expr) do { ++checks; if(!(expr)){ std::printf("FAIL: %s\n",#expr); ++failures;} } while(0)

// helpers to build Json message dicts
static Json mstr(const std::string& role, const std::string& content) {
    Json o = json_obj();
    obj_set(o, "role", json_str(role));
    obj_set(o, "content", json_str(content));
    return o;
}
static Json mmult(const std::vector<std::string>& texts) {
    Json o = json_obj();
    obj_set(o, "role", json_str("user"));
    Json arr = json_arr();
    for (const std::string& t : texts) {
        Json p = json_obj();
        obj_set(p, "type", json_str("text"));
        obj_set(p, "text", json_str(t));
        arr_push(arr, p);
    }
    obj_set(o, "content", arr);
    return o;
}

int main() {
    // messages fixtures
    std::vector<Json> ms_tools;
    ms_tools.push_back(mstr("user","audit the module"));
    ms_tools.push_back(mstr("tool","Error: boom"));
    ms_tools.push_back(mstr("tool","ok fine"));

    std::vector<Json> errs;
    errs.push_back(mstr("tool","  FAILED to do it  "));
    errs.push_back(mstr("tool","  timeout after 5s"));
    errs.push_back(mstr("tool","all good"));

    // _tool_stats
    {
        auto ts = _tool_stats(ms_tools);
        CHK(ts.first == 2 && ts.second == 1);
        auto es = _tool_stats(errs);
        CHK(es.first == 3 && es.second == 2);
    }

    // _text_of / _last_user_text (multimodal)
    {
        std::vector<Json> mm; mm.push_back(mmult({"part one","part two"}));
        CHK(_text_of(mm[0]) == "part one\npart two");
        CHK(_last_user_text(mm) == "part one\npart two");
        CHK(_last_user_text(ms_tools) == "audit the module");
    }

    // _build_hit / _task_hits
    CHK(_build_hit("build a website for X"));
    CHK(_build_hit("write /home/x app"));
    CHK(!_build_hit("generate something"));
    CHK(!_build_hit("ship it now"));
    CHK(_build_hit("create a database"));
    CHK(_task_hits("build a website for X", nullptr) == 2);
    CHK(_task_hits("write /home/x app", nullptr) == 2);
    CHK(_task_hits("generate something", nullptr) == 0);
    CHK(_task_hits("ship it now", nullptr) == 0);
    CHK(_task_hits("create a database", nullptr) == 2);

    // score_task
    CHK(score_task("audit the code") == 2);
    CHK(score_task("think hard about this bug") == 8);
    CHK(score_task("status", &ms_tools) == 4);
    CHK(score_task("please build a website for my startup") == 4);
    CHK(score_task("audit audit audit audit") == 8);
    CHK(score_task("/home/alca/x.py") == 2);
    CHK(score_task("refactor the whole project and add tests and docs and be thorough please and comprehensive") == 6);
    CHK(score_task("") == 0);
    CHK(score_task("hi") == 0);
    CHK(score_task("use maximum reasoning to fix") == 0);
    CHK(score_task("danke") == 0);

    // _task_hits spot checks
    CHK(_task_hits("audit the code", nullptr) == 1);
    CHK(_task_hits("audit audit audit audit", nullptr) == 4);
    CHK(_task_hits("think hard about this bug", nullptr) == 0);
    CHK(_task_hits("use maximum reasoning to fix", nullptr) == 0);

    // level_for_score(score, floor)
    CHK(level_for_score(50,"low") == "ultra");
    CHK(level_for_score(0,"high") == "high");
    CHK(level_for_score(42,"low") == "ultra");
    CHK(level_for_score(13,"medium") == "xhigh");
    CHK(level_for_score(14,"xhigh") == "max");
    CHK(level_for_score(7,"low") == "high");
    CHK(level_for_score(4,"low") == "medium");
    CHK(level_for_score(100,"ultra") == "ultra");

    // clamp_level
    CHK(clamp_level("low","xhigh") == "xhigh");
    CHK(clamp_level("bogus","medium") == "medium");
    CHK(clamp_level("max","low") == "max");
    CHK(clamp_level("ultra","ultra") == "ultra");
    // floor "l\xC3\xBCck" not in index -> low; level high stays
    CHK(clamp_level("high","l\xC3" "\xBC" "ck") == "high");
    CHK(clamp_level("medium","") == "medium");

    // estimate_level(user, messages, floor)
    CHK(estimate_level("") == "low");
    CHK(estimate_level("hi") == "low");
    CHK(estimate_level("audit the code") == "medium");
    CHK(estimate_level("refactor the whole project and add tests and docs and be thorough please and comprehensive") == "high");
    CHK(estimate_level("think hard about this bug") == "high");
    CHK(estimate_level("use maximum reasoning to fix") == "max");
    CHK(estimate_level("danke") == "low");
    CHK(estimate_level("status", &ms_tools) == "medium");
    CHK(estimate_level("please build a website for my startup") == "medium");
    CHK(estimate_level("danke", &ms_tools) == "low");
    CHK(estimate_level("ultra") == "max");
    CHK(estimate_level("audit audit audit audit") == "high");
    CHK(estimate_level("/home/alca/x.py") == "low");
    CHK(estimate_level("fix", nullptr, "high") == "high");

    if (failures == 0) std::printf("task_intensity: %d checks: PASS\n", checks);
    return failures;
}
