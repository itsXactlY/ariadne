// Differential test: agent/message_sanitization.py pure subset vs Python truth.
#include "dd/agent/message_sanitization.hpp"
#include <initializer_list>
#include <utility>
#include <string>
#include <vector>
#include "dd/core/json.hpp"
#include <iostream>

using namespace dd;
using namespace dd::agent;
using namespace dd::core;

static int g_fail = 0;
static void chk(const std::string& what, bool ok) {
    if (!ok) { std::cout << "FAIL " << what << "\n"; ++g_fail; }
}

static Json mkobj(std::initializer_list<std::pair<std::string, Json>> ms) {
    Json o = json_obj();
    for (auto& m : ms) obj_set(o, m.first, m.second);
    return o;
}

int main() {
    // ---- escape_invalid_chars_in_json_strings ----
    chk("esc:tab",   escape_invalid_chars_in_json_strings("{\"a\":\"x\ty\"}") == "{\"a\":\"x\\u0009y\"}");
    chk("esc:nl",    escape_invalid_chars_in_json_strings("{\"a\":\"line\nbreak\"}") == "{\"a\":\"line\\u000abreak\"}");
    chk("esc:ctrl2", escape_invalid_chars_in_json_strings("{\"k\":\"\x01\x1f\"}") == "{\"k\":\"\\u0001\\u001f\"}");
    chk("esc:passthru", escape_invalid_chars_in_json_strings("{\"a\":\"\\u0001\"}") == "{\"a\":\"\\u0001\"}");
    chk("esc:endslash", escape_invalid_chars_in_json_strings("{\"a\":\"end\\\\\"}") == "{\"a\":\"end\\\\\"}");
    chk("esc:outside", escape_invalid_chars_in_json_strings("a\"b\x07" "c\"d") == "a\"b\\u0007c\"d");
    chk("esc:empty", escape_invalid_chars_in_json_strings("") == "");
    chk("esc:plain", escape_invalid_chars_in_json_strings("{\"ok\":1}") == "{\"ok\":1}");
    // the ctrl expected line must be the literal-backslash form; ensure consistency:
    chk("ctrl-consistent", escape_invalid_chars_in_json_strings("{\"k\":\"\x01\x1f\"}") == "{\"k\":\"\\u0001\\u001f\"}");

    // ---- strip_non_ascii ----
    chk("strip:cafe", strip_non_ascii("caf\xC3\xA9") == "caf");
    chk("strip:naive", strip_non_ascii("na\xC3\xAFve \xE2\x80\x94 dash") == "nave  dash");
    chk("strip:ascii", strip_non_ascii("ascii") == "ascii");
    chk("strip:ctrl", strip_non_ascii("\x00\x1f") == "\x00\x1f");
    chk("strip:empty", strip_non_ascii("") == "");
    chk("strip:mixed", strip_non_ascii("a\xC3\xA9\xFF" "b") == "ab");

    // ---- close_interrupted_tool_sequence ----
    auto run_close = [](std::vector<Json> msgs, const std::string& fr) -> std::pair<bool, std::string> {
        bool r = close_interrupted_tool_sequence(msgs, fr);
        if (msgs.empty()) return {r, "<none>"};
        const Json& last = msgs.back();
        std::string c;
        if (last.type == Json::Obj) { if (const Json* cc = json_get(last,"content")) if (cc->type==Json::Str) c=cc->str; }
        return {r, c};
    };
    auto r0 = run_close({}, "x");
    chk("close:empty", r0.first==false && r0.second=="<none>");
    auto r1 = run_close({ mkobj({{"role",json_str("tool")},{"content",json_str("t")}}) }, "");
    chk("close:tool1", r1.first==true && r1.second=="Operation interrupted.");
    auto r2 = run_close({ mkobj({{"role",json_str("tool")}}) }, "  hello  ");
    chk("close:tool2", r2.first==true && r2.second=="hello");
    auto r3 = run_close({ mkobj({{"role",json_str("user")}}) }, "x");
    chk("close:user", r3.first==false && r3.second=="");
    auto r4 = run_close({ mkobj({{"role",json_str("tool")}}) }, "");
    chk("close:tool3", r4.first==true && r4.second=="Operation interrupted.");

    if (g_fail) { std::cout << "message_sanitization: FAIL " << g_fail << "\n"; return 1; }
    std::cout << "all message_sanitization tests passed\n";
    return 0;
}
