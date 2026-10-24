// tests/json_test.cpp — what goes on the wire is always well-formed UTF-8.
#include "dd/core/json.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace dd::core;

static int g_fail = 0, g_pass = 0;
static void chk(bool cond, const std::string& what) {
    if (cond) { ++g_pass; } else { ++g_fail; std::cout << "FAIL: " << what << "\n"; }
}

static std::string str_of(const std::string& json) { return json_parse(json).str; }

int main() {
    // Python's json.dumps escapes emoji as a surrogate pair: one code point, 4 bytes
    chk(str_of(R"("🔴")") == "\xF0\x9F\x94\xB4", "surrogate pair -> U+1F534");
    chk(str_of(R"("[🧱] x")") == "[\xF0\x9F\xA7\xB1] x", "pair inside text");
    chk(str_of(R"("😀")") == "\xF0\x9F\x98\x80", "upper-case hex");
    chk(str_of(R"("ä—→")") == "\xC3\xA4\xE2\x80\x94\xE2\x86\x92", "BMP unchanged");
    chk(str_of(R"("a\ud83db")") == "a\xEF\xBF\xBD" "b", "lone high surrogate -> U+FFFD");
    chk(str_of(R"("a\ude00b")") == "a\xEF\xBF\xBD" "b", "lone low surrogate -> U+FFFD");
    chk(str_of(R"("\ud83dA")") == "\xEF\xBF\xBD" "A", "high + non-low: both kept apart");
    bool threw = false;
    try { json_parse(R"("\u12g4")"); } catch (...) { threw = true; }
    chk(threw, "bad hex throws");

    auto dump_str = [](const std::string& s) { return json_dump(json_str(s)); };
    chk(dump_str("\xF0\x9F\x94\xB4 \xC3\xA4") == "\"\xF0\x9F\x94\xB4 \xC3\xA4\"", "valid UTF-8 passes through");
    chk(dump_str("ab\xE2\x80") == "\"ab\xEF\xBF\xBD\xEF\xBF\xBD\"", "cut sequence -> U+FFFD per byte");
    chk(dump_str("\xED\xA0\xBD") == "\"\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\"", "CESU surrogate bytes rejected");
    chk(dump_str("\xC0\xAF") == "\"\xEF\xBF\xBD\xEF\xBF\xBD\"", "overlong rejected");
    chk(dump_str("\xF4\x90\x80\x80") == "\"\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\"", "past U+10FFFF rejected");
    chk(dump_str("\xFF" "x") == "\"\xEF\xBF\xBD" "x\"", "stray byte");
    chk(dump_str("q\"\\\n\x01") == "\"q\\\"\\\\\\n\\u0001\"", "escapes unchanged");

    // the 500 from 2026-10-20: a recall result with an escaped emoji, round-tripped
    const std::string wire = R"({"text":"[{\"content\": \"[🔴] × curl\"}]"})";
    const std::string out = json_dump(json_parse(wire));
    chk(out.find("\xF0\x9F\x94\xB4") != std::string::npos && out.find("\xED") == std::string::npos, "recall round trip");

    // the write_file crash of 2026-10-20: the model ran out of output budget
    // mid-string, so the arguments arrive cut; salvage, never a half-value.
    bool rep = false;
    const Json cut = json_parse_args(R"({"path":"/x/y.md","content":"# a book)", rep);
    chk(rep && cut.type == Json::Obj, "truncated args -> repaired object");
    chk(json_get(cut, "path") && json_get(cut, "path")->str == "/x/y.md", "the finished member survives");
    chk(json_get(cut, "content") == nullptr, "the cut member is dropped, never half-written");
    rep = false;
    const Json nested = json_parse_args(R"({"m":{"k":1},"x":"y)", rep);
    chk(rep && nested.type == Json::Obj && json_get(nested, "m"), "a finished nested member survives");
    rep = false;
    const Json whole = json_parse_args(R"({"cmd":"ls"})", rep);
    chk(!rep && whole.type == Json::Obj, "well-formed args parse as-is");
    rep = false;
    const Json junk = json_parse_args("{oops", rep);
    chk(rep && junk.type == Json::Null, "unrecoverable -> Null, no throw");

    // ---- nesting: bounded, so no input can overflow the stack (50k levels crashed the parser) ----
    {
        auto arr = [](size_t n) { return std::string(n, '[') + std::string(n, ']'); };
        auto obj = [](size_t n) { std::string s; for (size_t i = 0; i < n; ++i) s += "{\"a\":"; s += "1"; return s + std::string(n, '}'); };
        auto parses = [](const std::string& t) { try { json_parse(t); return true; } catch (const std::runtime_error&) { return false; } };
        chk(parses(arr(200)) && parses(obj(200)), "200 levels parse");
        chk(parses(arr(kJsonMaxDepth)) && parses(obj(kJsonMaxDepth)), "kJsonMaxDepth levels parse");
        chk(!parses(arr(kJsonMaxDepth + 1)) && !parses(obj(kJsonMaxDepth + 1)), "one more is refused");
        chk(!parses(arr(50000)) && !parses(obj(50000)), "50k levels throw instead of crashing");
        bool rep = false;
        const Json deep = json_parse_args(std::string(50000, '['), rep);
        chk(rep && deep.type == Json::Null, "too-deep args -> Null, repaired, no throw");
        rep = false;
        const Json deep_obj = json_parse_args(obj(50000), rep);
        chk(rep && deep_obj.type == Json::Null, "too-deep object args -> Null");
        rep = false;
        const Json cut_nested = json_parse_args("{\"keep\":1,\"a\":" + obj(150).substr(0, 400), rep);   // a nested call cut short still salvages
        chk(rep && cut_nested.type == Json::Obj && json_get(cut_nested, "keep") && !json_get(cut_nested, "a"), "cut but not deep: salvaged");
        const std::string round = json_dump(json_parse(obj(200)));
        chk(round == obj(200), "200-deep round trip");

        // a tree built by hand, far past the limit: dump and deep copy cut it, neither overflows
        Json cur = json_arr();
        for (int i = 0; i < 5000; ++i) { Json up = json_arr(); arr_push(up, cur); cur = std::move(up); }
        const std::string d = json_dump(cur);
        size_t open = 0;
        while (open < d.size() && d[open] == '[') ++open;
        chk(open == static_cast<size_t>(kJsonMaxDepth) + 1 && d.find("null") != std::string::npos, "dump cuts a hand-built deep tree to null");
        const Json copy = json_deep_copy(cur);
        int levels = 0;
        for (const Json* p = &copy; p->type == Json::Arr && !p->arr.empty(); p = p->arr[0].get()) ++levels;
        chk(levels == kJsonMaxDepth + 1, "deep copy cuts at the same depth");
    }

    // ---- numbers follow the JSON grammar ----
    {
        auto num = [](const std::string& t, double want) { try { const Json j = json_parse(t); return j.type == Json::Num && j.num == want; } catch (...) { return false; } };
        auto bad = [](const std::string& t) { try { json_parse(t); return false; } catch (const std::runtime_error&) { return true; } };
        chk(num("0", 0) && num("-0", 0) && num("12", 12) && num("-12.5", -12.5) && num("1e3", 1000) && num("1E+3", 1000) && num("-12.5e-1", -1.25), "valid numbers");
        chk(bad("+1") && bad("-") && bad("1.") && bad(".5") && bad("1e") && bad("1e+") && bad("--1") && bad("x"), "invalid numbers throw");
        chk(bad("[1-2]") && bad("[01]") && bad("[1,]") && bad("[+1]"), "inside a container the grammar is strict");
        chk(bad("1e999") && bad("-1e999") && bad("[1e999]"), "overflow throws runtime_error");
        chk(num("1e-999", 0), "underflow is zero");
        chk(num("123456789012345678", 123456789012345678.0), "big integers");
        chk(json_parse("1-2-3").num == 1 && json_parse(R"({"a":1} trailing)").type == Json::Obj, "text after the first value is ignored");
        chk(json_dump(json_parse("[1,2.5,-3e2,0.1]")) == "[1,2.5,-300,0.10000000000000001]", "numbers dump");
    }

    std::cout << "json: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail ? 1 : 0;
}
