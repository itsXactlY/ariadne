// Differential test: agent/context_references.py pure subset vs Python truth.
#include "dd/agent/context_references.hpp"
#include <iostream>
#include <string>

using namespace dd;
using namespace dd::agent;

static int g_fail = 0;
static void chk(const std::string& what, bool ok) {
    if (!ok) { std::cout << "FAIL " << what << "\n"; ++g_fail; }
}

static std::string sline(const ContextReference& r) {
    std::string s = r.raw + "|" + r.kind + "|" + r.target + "|" +
                    std::to_string(r.start) + "|" + std::to_string(r.end);
    s += "|";
    if (r.line_start) s += std::to_string(*r.line_start);
    s += "|";
    if (r.line_end) s += std::to_string(*r.line_end);
    return s;
}

// Parse one message -> vector of serialized refs joined by '\n'.
static std::string refs_join(const std::string& msg) {
    std::string out;
    for (const auto& r : parse_context_references(msg)) {
        if (!out.empty()) out += "\n";
        out += sline(r);
    }
    return out;
}

int main() {
    // ---- parse_context_references ----
    // "@file:src/foo.py:10-20\n@diff\n@staged"
    chk("p1", refs_join("@file:src/foo.py:10-20\n@diff\n@staged") ==
        "@file:src/foo.py:10-20|file|src/foo.py|0|22|10|20\n"
        "@diff|diff||23|28||\n"
        "@staged|staged||29|36||");
    // "see @file:/home/a/b.txt:5 @folder:x @git:y @url:http://x "
    chk("p2", refs_join("see @file:/home/a/b.txt:5 @folder:x @git:y @url:http://x ") ==
        "@file:/home/a/b.txt:5|file|/home/a/b.txt|4|25|5|5\n"
        "@folder:x|folder|x|26|35||\n"
        "@git:y|git|y|36|42||\n"
        "@url:http://x|url|http://x|43|56||");
    chk("p3", refs_join("x@file:ignored") == "");
    chk("p4", refs_join("/@file:y") == "");
    chk("p5", refs_join("@file:foo.py:") == "@file:foo.py:|file|foo.py:|0|13||");
    chk("p6", refs_join("@file:a.py:10") == "@file:a.py:10|file|a.py|0|13|10|10");
    chk("p7", refs_join("hello @diff") == "@diff|diff||6|11||");
    chk("p8", refs_join("@diffx") == "");
    chk("p9", refs_join("@file:a.py. ") == "@file:a.py.|file|a.py|0|11||");
    chk("p10", refs_join("@file:a.py)") == "@file:a.py)|file|a.py|0|11||");
    chk("p11", refs_join("@file:(a.py)") == "@file:(a.py)|file|(a.py)|0|12||");
    chk("p12", refs_join("@url:https://x.com/a?b=1") == "@url:https://x.com/a?b=1|url|https://x.com/a?b=1|0|24||");
    chk("p13", refs_join("@file:x.py:1-2-3") == "@file:x.py:1-2-3|file|x.py:1-2-3|0|16||");
    chk("p14", refs_join("@staged!") == "@staged|staged||0|7||");
    chk("p15", refs_join("@diff,more") == "@diff|diff||0|5||");
    chk("p_empty", refs_join("") == "");

    // ---- strip_trailing_punctuation ----
    chk("strip:plain", strip_trailing_punctuation("a.py") == "a.py");
    chk("strip:dot", strip_trailing_punctuation("a.py.") == "a.py");
    chk("strip:parens", strip_trailing_punctuation("a.py))") == "a.py");
    chk("strip:balanced", strip_trailing_punctuation("(a.py)") == "(a.py)");
    chk("strip:braces", strip_trailing_punctuation("x}}") == "x");
    chk("strip:onebrace", strip_trailing_punctuation("{x}}") == "{x}");
    chk("strip:bang", strip_trailing_punctuation("a.py!?") == "a.py");

    // ---- code_fence_language ----
    chk("fence:py", code_fence_language(".py") == "python");
    chk("fence:PY", code_fence_language(".PY") == "python");
    chk("fence:ts", code_fence_language(".ts") == "typescript");
    chk("fence:tsx", code_fence_language(".tsx") == "tsx");
    chk("fence:sh", code_fence_language(".sh") == "bash");
    chk("fence:yaml", code_fence_language(".yaml") == "yaml");
    chk("fence:txt", code_fence_language(".txt") == "");
    chk("fence:none", code_fence_language("") == "");

    // ---- remove_reference_tokens ----
    chk("rm1", remove_reference_tokens("a @file:x.py b @diff c", parse_context_references("a @file:x.py b @diff c")) ==
        "a b c");
    chk("rm2", remove_reference_tokens("keep @staged here", parse_context_references("keep @staged here")) ==
        "keep here");
    chk("rm3", remove_reference_tokens("x  @diff  y", parse_context_references("x  @diff  y")) ==
        "x y");
    chk("rm4", remove_reference_tokens("@diff, tail", parse_context_references("@diff, tail")) == ", tail");

    if (g_fail) { std::cout << "context_references: FAIL " << g_fail << "\n"; return 1; }
    std::cout << "all context_references tests passed\n";
    return 0;
}
