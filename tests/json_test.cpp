// tests/json_test.cpp — what goes on the wire is always well-formed UTF-8.
#include "dd/core/json.hpp"

#include <iostream>
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

    std::cout << "json: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail ? 1 : 0;
}
