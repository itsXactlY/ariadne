// Ground-truth cross-checks for the fuzzy_match port (verified against
// Python tools/fuzzy_match.py in-session).
#include <iostream>
#include <string>
#include <optional>
#include <vector>
#include "dd/tools/fuzzy_match.hpp"

using namespace dd;
using namespace dd::tools;

static int g_fail = 0;

static std::string qstr(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '\n') {
            out += "\\n";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c == '"') {
            out += "\\\"";
        } else {
            out += c;
        }
    }
    out += "\"";
    return out;
}

static void show(const FuzzyResult& r) {
    std::cout << "content=" << qstr(r.content) << " mc=" << r.match_count
              << " err=" << (r.error ? qstr(*r.error) : std::string("null")) << "\n";
}

#define CHECK(cond, label)                                                    \
    do {                                                                      \
        if (!(cond)) {                                                        \
            ++g_fail;                                                         \
            std::cout << "FAIL " << (label) << "\n";                          \
        }                                                                     \
    } while (0)

int main() {
    // 1 exact
    {
        FuzzyResult r = fuzzy_find_and_replace("def foo():\n    return 1\n", "    return 1", "    return 2");
        show(r);
        CHECK(r.content == "def foo():\n    return 2\n", "1.content");
        CHECK(r.match_count == 1, "1.mc");
        CHECK(!r.error, "1.err-null");
    }
    // 2 line_trimmed (trailing spaces preserved in output)
    {
        std::string c = "def foo():\n    x = 1   \n    y = 2\n";
        FuzzyResult r = fuzzy_find_and_replace(c, "    x = 1", "    x = 9");
        show(r);
        CHECK(r.content == "def foo():\n    x = 9   \n    y = 2\n", "2.content");
        CHECK(r.match_count == 1, "2.mc");
        CHECK(!r.error, "2.err");
        FuzzyResult r2 = fuzzy_find_and_replace(c, "x = 1", "x = 9");
        show(r2);
        CHECK(r2.content == "def foo():\n    x = 9   \n    y = 2\n", "2b.content");
        CHECK(r2.match_count == 1, "2b.mc");
        CHECK(!r2.error, "2b.err");
    }
    // 3 whitespace_normalized
    {
        FuzzyResult r = fuzzy_find_and_replace("a  =   1\nb = 2\n", "a = 1", "a = 99");
        show(r);
        CHECK(r.content == "a = 99\nb = 2\n", "3.content");
        CHECK(r.match_count == 1, "3.mc");
        CHECK(!r.error, "3.err");
    }
    // 4 indentation_flexible
    {
        FuzzyResult r = fuzzy_find_and_replace("def f():\n    if True:\n        return 1\n",
                                               "if True:\nreturn 1", "if False:\nreturn 0");
        show(r);
        CHECK(r.content == "def f():\nif False:\nreturn 0\n", "4.content");
        CHECK(r.match_count == 1, "4.mc");
        CHECK(!r.error, "4.err");
    }
    // 5 escape_normalized (pattern has literal backslash-n)
    {
        FuzzyResult r = fuzzy_find_and_replace("line1\nline2\n", "line1\\nline2", "REPLACED");
        show(r);
        CHECK(r.content == "REPLACED\n", "5.content");
        CHECK(r.match_count == 1, "5.mc");
        CHECK(!r.error, "5.err");
    }
    // 6 trimmed_boundary
    {
        FuzzyResult r = fuzzy_find_and_replace("HEADER\n    body 1\n    body 2\nFOOTER\n",
                                               "HEADER\n    body 1\n    body 2\nFOOTER", "NEWBLOCK");
        show(r);
        CHECK(r.content == "NEWBLOCK\n", "6.content");
        CHECK(r.match_count == 1, "6.mc");
        CHECK(!r.error, "6.err");
    }
    // 7 block_anchor (first/last match, middle differs, sim 0.9130 >= 0.30)
    {
        FuzzyResult r = fuzzy_find_and_replace("FIRST\n    aaa = 1\n    bbb = 2\nLAST\n",
                                               "FIRST\n    aaa = 9\n    bbb = 8\nLAST", "ZAP");
        show(r);
        CHECK(r.content == "ZAP\n", "7.content");
        CHECK(r.match_count == 1, "7.mc");
        CHECK(!r.error, "7.err");
    }
    // 8 ambiguous (two identical lines, no replace_all)
    {
        FuzzyResult r = fuzzy_find_and_replace("x = 1\ny = 2\nx = 1\n", "x = 1", "X");
        show(r);
        CHECK(r.content == "x = 1\ny = 2\nx = 1\n", "8.content-unchanged");
        CHECK(r.match_count == 0, "8.mc");
        CHECK(r.error && *r.error ==
                  "Found 2 matches for old_string. Provide more context to make it unique, or use replace_all=True.",
              "8.err");
    }
    // 8b replace_all
    {
        FuzzyResult r = fuzzy_find_and_replace("x = 1\ny = 2\nx = 1\n", "x = 1", "X", true);
        show(r);
        CHECK(r.content == "X\ny = 2\nX\n", "8b.content");
        CHECK(r.match_count == 2, "8b.mc");
        CHECK(!r.error, "8b.err");
    }
    // empty old_string
    {
        FuzzyResult r = fuzzy_find_and_replace("abc", "", "z");
        show(r);
        CHECK(r.content == "abc", "empty.content");
        CHECK(r.match_count == 0, "empty.mc");
        CHECK(r.error && *r.error == "old_string cannot be empty", "empty.err");
    }
    // identical
    {
        FuzzyResult r = fuzzy_find_and_replace("abc", "a", "a");
        show(r);
        CHECK(r.content == "abc", "ident.content");
        CHECK(r.match_count == 0, "ident.mc");
        CHECK(r.error && *r.error == "old_string and new_string are identical", "ident.err");
    }
    // no match
    {
        FuzzyResult r = fuzzy_find_and_replace("abc", "zzz", "q");
        show(r);
        CHECK(r.content == "abc", "nomatch.content");
        CHECK(r.match_count == 0, "nomatch.mc");
        CHECK(r.error && *r.error == "Could not find a match for old_string in the file", "nomatch.err");
    }

    if (g_fail == 0) {
        std::cout << "all fuzzy-match tests passed\n";
        return 0;
    }
    std::cout << g_fail << " fuzzy-match test(s) failed\n";
    return 1;
}
