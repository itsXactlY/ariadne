// Differential test for daedalus_state scrub cluster vs captured Python truth.
#include <iostream>
#include <string>
#include <vector>

#include "dd/core/json.hpp"
#include "dd/core/state_scrub.hpp"

using namespace dd;
using namespace dd::core;

static int failures = 0;
static void chk(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::cout << "FAIL " << what << "\n";
    }
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

static std::string content_of(const Json& m) {
    const Json* c = json_get(m, "content");
    return (c && c->type == Json::Str) ? c->str : std::string("<none>");
}
static std::string role_of(const Json& m) {
    const Json* r = json_get(m, "role");
    return (r && r->type == Json::Str) ? r->str : std::string("<none>");
}

int main() {
    // ---- _is_stale_tool_call_marker_message ----
    chk(_is_stale_tool_call_marker_message(
            mkobj({{"role", json_str("assistant")}, {"tool_calls", mkarr({json_obj()})},
                   {"content", json_str("  [memory] ")}})),
        "stale:blank-marker");
    chk(!_is_stale_tool_call_marker_message(
            mkobj({{"role", json_str("assistant")}, {"tool_calls", mkarr({})},
                   {"content", json_str("[x]")}})),
        "stale:empty-toolcalls");
    chk(!_is_stale_tool_call_marker_message(
            mkobj({{"role", json_str("assistant")}, {"content", json_str("[foo]")}})),
        "stale:notoolcalls");
    chk(_is_stale_tool_call_marker_message(
            mkobj({{"role", json_str("assistant")}, {"tool_calls", mkarr({json_obj()})},
                   {"content", json_str("[a-b.c]")}})),
        "stale:hyphen-dot");
    chk(!_is_stale_tool_call_marker_message(
            mkobj({{"role", json_str("assistant")}, {"tool_calls", mkarr({json_obj()})},
                   {"content", json_str("[9bad]")}})),
        "stale:digits-first");
    chk(!_is_stale_tool_call_marker_message(
            mkobj({{"role", json_str("assistant")}, {"tool_calls", mkarr({json_obj()})},
                   {"content", json_str("plain")}})),
        "stale:plain");
    chk(!_is_stale_tool_call_marker_message(
            mkobj({{"role", json_str("user")}, {"tool_calls", mkarr({json_obj()})},
                   {"content", json_str("[x]")}})),
        "stale:user-role");

    // ---- _is_background_review_harness_message ----
    chk(_is_background_review_harness_message(
            mkobj({{"role", json_str("user")},
                   {"content", json_str("  Review the conversation above and update the skill library now")}})),
        "rh:user-prefix");
    chk(_is_background_review_harness_message(
            mkobj({{"role", json_str("system")},
                   {"content", json_str("Review the conversation above and consider saving to memory?")}})),
        "rh:system-prefix");
    chk(!_is_background_review_harness_message(
            mkobj({{"role", json_str("assistant")},
                   {"content", json_str("Review the conversation above and update the skill library")}})),
        "rh:assistant-role");

    // ---- strip_stale contents ----
    {
        std::vector<Json> in{
            mkobj({{"role", json_str("assistant")}, {"tool_calls", mkarr({json_obj()})},
                   {"content", json_str("  [memory] ")}}),
            mkobj({{"role", json_str("assistant")}, {"tool_calls", mkarr({})},
                   {"content", json_str("[x]")}}),
            mkobj({{"role", json_str("assistant")}, {"content", json_str("[foo]")}}),
            mkobj({{"role", json_str("assistant")}, {"tool_calls", mkarr({json_obj()})},
                   {"content", json_str("[a-b.c]")}}),
            mkobj({{"role", json_str("assistant")}, {"tool_calls", mkarr({json_obj()})},
                   {"content", json_str("[9bad]")}}),
            mkobj({{"role", json_str("assistant")}, {"tool_calls", mkarr({json_obj()})},
                   {"content", json_str("plain")}})};
        auto out = _strip_stale_tool_call_markers(in);
        const std::vector<std::string> want{"", "[x]", "[foo]", "", "[9bad]", "plain"};
        chk(out.size() == want.size(), "strip-stale:size");
        for (std::size_t i = 0; i < want.size(); ++i) chk(content_of(out[i]) == want[i], "strip-stale:" + std::to_string(i));
    }

    // ---- strip_rh roles ----
    {
        std::vector<Json> in{
            mkobj({{"role", json_str("assistant")}, {"tool_calls", mkarr({json_obj()})},
                   {"content", json_str("  [memory] ")}}),
            mkobj({{"role", json_str("assistant")}, {"tool_calls", mkarr({})},
                   {"content", json_str("[x]")}}),
            mkobj({{"role", json_str("assistant")}, {"content", json_str("[foo]")}}),
            mkobj({{"role", json_str("assistant")}, {"tool_calls", mkarr({json_obj()})},
                   {"content", json_str("[a-b.c]")}}),
            mkobj({{"role", json_str("assistant")}, {"tool_calls", mkarr({json_obj()})},
                   {"content", json_str("[9bad]")}}),
            mkobj({{"role", json_str("assistant")}, {"tool_calls", mkarr({json_obj()})},
                   {"content", json_str("plain")}}),
            mkobj({{"role", json_str("user")},
                   {"content", json_str("  Review the conversation above and update the skill library now")}}),
            mkobj({{"role", json_str("system")},
                   {"content", json_str("Review the conversation above and consider saving to memory?")}}),
            mkobj({{"role", json_str("assistant")}, {"content", json_str("curator reply")}})};
        auto out = _strip_background_review_harness(in);
        const std::vector<std::string> want{"assistant", "assistant", "assistant",
                                            "assistant", "assistant", "assistant"};
        chk(out.size() == want.size(), "strip-rh:size");
        for (std::size_t i = 0; i < want.size(); ++i) chk(role_of(out[i]) == want[i], "strip-rh:" + std::to_string(i));
    }

    // ---- rh-walk ordering ----
    {
        std::vector<Json> walk{
            mkobj({{"role", json_str("user")},
                   {"content", json_str("Review the conversation above and update the skill library")}}),
            mkobj({{"role", json_str("assistant")}, {"content", json_str("curator")}}),
            mkobj({{"role", json_str("user")}, {"content", json_str("hi")}}),
            mkobj({{"role", json_str("assistant")}, {"content", json_str("yo")}}),
            mkobj({{"role", json_str("system")},
                   {"content", json_str("Review the conversation above and consider saving to memory")}})};
        auto out = _strip_background_review_harness(walk);
        chk(out.size() == 2, "rh-walk:size");
        chk(out.size() == 2 && role_of(out[0]) == "user" && role_of(out[1]) == "assistant", "rh-walk:order");
    }

    // ---- _is_production_state_db (Path.parts algebra) ----
    chk(_is_production_state_db({"/", "h", "state.db"}, {"/", "h"}), "prod:direct");
    chk(_is_production_state_db({"/", "h", "profiles", "p1", "state.db"}, {"/", "h"}), "prod:profile");
    chk(!_is_production_state_db({"/", "h", "a", "b", "c.db"}, {"/", "h"}), "prod:deeper");
    chk(!_is_production_state_db({"/", "other", "x"}, {"/", "h"}), "prod:outsider");
    chk(!_is_production_state_db({"/", "h", "x", "y", "z.db"}, {"/", "h"}), "prod:not-profile");

    if (failures == 0) {
        std::cout << "all state-scrub tests passed\n";
        return 0;
    }
    std::cout << failures << " state-scrub tests FAILED\n";
    return 1;
}
