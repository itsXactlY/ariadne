// Verifies dd::tools DebugSession.
#include "dd/tools/debug_helpers.hpp"
#include "dd/core/json.hpp"
#include "dd/core/home.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using namespace dd;
using namespace dd::tools;
using namespace core;

static int failures = 0;
#define CHECK(cond, msg)                                   \
    do {                                                   \
        if (!(cond)) {                                     \
            std::cerr << "FAIL: " << msg << "\n";          \
            ++failures;                                    \
        }                                                  \
    } while (0)

int main() {
    // --- disabled session (env var unset) ---
    {
        DebugSession ds("web_tools", "DD_DBG_UNSET");
        CHECK(!ds.active(), "disabled when env unset");
        Json info = ds.get_session_info();
        const Json* en = json_get(info, "enabled");
        CHECK(en && en->type == Json::Bool && !en->b, "enabled=false");
        const Json* sid = json_get(info, "session_id");
        CHECK(sid && sid->type == Json::Null, "session_id null");
        const Json* lp = json_get(info, "log_path");
        CHECK(lp && lp->type == Json::Null, "log_path null");
        const Json* tc = json_get(info, "total_calls");
        CHECK(tc && tc->type == Json::Num && tc->num == 0, "total_calls 0");
        ds.log_call("x", json_obj());  // no-op
        ds.save();                     // no-op
    }

    // --- enabled session (env var = true) ---
    setenv("DD_DBG", "true", 1);
    {
        DebugSession ds("web_tools", "DD_DBG");
        CHECK(ds.active(), "enabled when env true");

        Json data = json_obj();
        obj_set(data, "query", json_str("hello"));
        obj_set(data, "results", json_num(3.0));
        ds.log_call("web_search", data);
        ds.log_call("web_fetch", json_obj());

        Json info = ds.get_session_info();
        const Json* en = json_get(info, "enabled");
        CHECK(en && en->type == Json::Bool && en->b, "enabled=true");
        const Json* tc = json_get(info, "total_calls");
        CHECK(tc && tc->type == Json::Num && tc->num == 2, "total_calls 2");
        const Json* lp = json_get(info, "log_path");
        CHECK(lp && lp->type == Json::Str && !lp->str.empty(), "log_path set");
        const Json* sid0 = json_get(info, "session_id");
        std::string sid_str = (sid0 && sid0->type == Json::Str) ? sid0->str : "";
        CHECK(!sid_str.empty(), "session_id set");

        ds.save();
        // read back the saved file and verify structure
        std::filesystem::path p(lp->str);
        std::ifstream ifs(p);
        CHECK((bool)ifs, "saved file exists");
        std::stringstream ss;
        ss << ifs.rdbuf();
        Json back = json_parse(ss.str());
        const Json* sid = json_get(back, "session_id");
        CHECK(sid && sid->type == Json::Str && sid->str == sid_str, "session_id round-trip");
        const Json* de = json_get(back, "debug_enabled");
        CHECK(de && de->type == Json::Bool && de->b, "debug_enabled true");
        const Json* tcs = json_get(back, "tool_calls");
        CHECK(tcs && tcs->type == Json::Arr && tcs->arr.size() == 2, "tool_calls 2 entries");
        if (tcs && tcs->arr.size() == 2) {
            const Json& e0 = *tcs->arr[0];
            const Json* tn = json_get(e0, "tool_name");
            CHECK(tn && tn->type == Json::Str && tn->str == "web_search", "entry0 tool_name");
            const Json* q = json_get(e0, "query");
            CHECK(q && q->type == Json::Str && q->str == "hello", "spread query key present");
            const Json* r = json_get(e0, "results");
            CHECK(r && r->type == Json::Num && r->num == 3, "spread results key present");
            const Json& e1 = *tcs->arr[1];
            const Json* tn1 = json_get(e1, "tool_name");
            CHECK(tn1 && tn1->str == "web_fetch", "entry1 tool_name");
        }
    }

    if (failures) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "all debug-helpers tests passed\n";
    return 0;
}
