// Differential test for pure CLI-text heuristics (_looks_like_slash_command,
// _split_path_input, path_is_within_root) vs captured Python truth.
#include <iostream>
#include <string>
#include <vector>
#include <cmath>
#include <optional>

#include "dd/cli/heuristics.hpp"

using namespace dd;
using namespace dd::cli;

static int failures = 0;
static void chk(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::cout << "FAIL " << what << "\n";
    }
}
static void chk_opt_lum(const std::string& what, const std::optional<double>& got, const std::optional<double>& want) {
    if (got.has_value() != want.has_value()) chk(false, what);
    else if (got.has_value() && std::fabs(*got - *want) > 1e-9) chk(false, what);
    else chk(true, what);
}
using P = std::vector<std::string>;

int main() {
    // ---- _looks_like_slash_command ----
    chk(_looks_like_slash_command("/help"), "slash:help");
    chk(_looks_like_slash_command("/model gpt-4"), "slash:model");
    chk(_looks_like_slash_command("/a"), "slash:a");
    chk(_looks_like_slash_command("/"), "slash:bare");
    chk(!_looks_like_slash_command(""), "slash:empty");
    chk(!_looks_like_slash_command("hello"), "slash:hello");
    chk(!_looks_like_slash_command("/Users/iron/file.md:45-46 hi"), "slash:path");
    chk(!_looks_like_slash_command("//x"), "slash:double");

    // ---- split_path_input ----
    {
        auto r = split_path_input("");
        chk(r.first == "" && r.second == "", "split:empty");
    }
    {
        auto r = split_path_input("   ");
        chk(r.first == "" && r.second == "", "split:blank");
    }
    {
        auto r = split_path_input("/a/b.txt");
        chk(r.first == "/a/b.txt" && r.second == "", "split:plain");
    }
    {
        auto r = split_path_input("/a/b.txt extra text");
        chk(r.first == "/a/b.txt" && r.second == "extra text", "split:tail");
    }
    {
        auto r = split_path_input("\"a b c.txt\" tail");
        chk(r.first == "a b c.txt" && r.second == "tail", "split:dq");
    }
    {
        auto r = split_path_input("'a b' tail x");
        chk(r.first == "a b" && r.second == "tail x", "split:sq");
    }
    {
        auto r = split_path_input("\"unclosed");
        chk(r.first == "unclosed" && r.second == "", "split:dq-unclosed");
    }
    {
        auto r = split_path_input("'unclosed tail");
        chk(r.first == "unclosed tail" && r.second == "", "split:sq-unclosed");
    }
    {
        auto r = split_path_input("a\\ b c.txt rest");
        chk(r.first == "a b" && r.second == "c.txt rest", "split:esc-space");
    }
    {
        auto r = split_path_input("no-space");
        chk(r.first == "no-space" && r.second == "", "split:nospace");
    }
    {
        auto r = split_path_input("\"a\\\"b\" tail");
        chk(r.first == "a\\\"b" && r.second == "tail", "split:esc-quote");
    }
    {
        auto r = split_path_input("  leading trim  ");
        chk(r.first == "leading" && r.second == "trim", "split:trim");
    }
    {
        auto r = split_path_input("\"quoted\" ");
        chk(r.first == "quoted" && r.second == "", "split:dq-trail");
    }
    {
        auto r = split_path_input("\"\" tail");
        chk(r.first == "" && r.second == "tail", "split:empty-dq");
    }

    // ---- path_is_within_root ----
    chk(path_is_within_root({"/", "a", "b", "c"}, {"/", "a", "b"}), "wr:sub");
    chk(!path_is_within_root({"/", "a", "b"}, {"/", "a", "b", "c"}), "wr:too-deep");
    chk(path_is_within_root({"/", "a", "b"}, {"/", "a", "b"}), "wr:equal");
    chk(path_is_within_root({"/", "a"}, {"/"}), "wr:under-root");
    chk(!path_is_within_root({"/", "a", "b"}, {"/", "c"}), "wr:sibling");
    chk(path_is_within_root({"/"}, {"/"}), "wr:root-root");
    chk(!path_is_within_root({"/", "a"}, {"/", "a", "b"}), "wr:shallow");
    chk(path_is_within_root({"/", "x", "y", "z"}, {"/", "x", "y"}), "wr:deep-sub");


    // ---- _collapse_blank_line_floods ----
    chk(_collapse_blank_line_floods("") == "", "cl:empty");
    chk(_collapse_blank_line_floods("a\n\n\n\nb") == "a\n\nb", "cl:interior");
    chk(_collapse_blank_line_floods("a\n\n\n\nb\n\n\n") == "a\n\nb", "cl:trailing");
    chk(_collapse_blank_line_floods("```\n\n\nx\n\n\n```") == "```\n\n\nx\n\n\n```", "cl:fence-preserve");
    chk(_collapse_blank_line_floods("```x\n\n\ny\n```") == "```x\n\n\ny\n```", "cl:fence-open");
    chk(_collapse_blank_line_floods("a\r\nb\n\n") == "a\r\nb", "cl:crlf");
    chk(_collapse_blank_line_floods("  \n  \n") == "", "cl:ws-only");
    chk(_collapse_blank_line_floods("```") == "```", "cl:bare-fence");
    chk(_collapse_blank_line_floods("\n") == "", "cl:one-nl");
    chk(_collapse_blank_line_floods("line\n   \n\t\nline") == "line\n\nline", "cl:ws-blanks");
    chk(_collapse_blank_line_floods("a\n~~~\n\n\n\nb\n~~~\n\n") == "a\n~~~\n\n\n\nb\n~~~", "cl:tilde-fence");
    chk(_collapse_blank_line_floods("keep\n\n\n") == "keep", "cl:trailing-blank");
    chk(_collapse_blank_line_floods("\n\na") == "\na", "cl:leading-blank");

    // ---- _luminance_from_hex ----
    chk_opt_lum("lum:#fff", _luminance_from_hex("#fff"), {0.9999999999999999});
    chk_opt_lum("lum:#ffffff", _luminance_from_hex("#ffffff"), {0.9999999999999999});
    chk_opt_lum("lum:#000000", _luminance_from_hex("#000000"), {0.0});
    chk_opt_lum("lum:#268bd2", _luminance_from_hex("#268bd2"), {0.48099450980392155});
    chk_opt_lum("lum:#abc", _luminance_from_hex("#abc"), {0.7239733333333334});
    chk_opt_lum("lum:abc", _luminance_from_hex("abc"), {0.7239733333333334});
    chk_opt_lum("lum:pad", _luminance_from_hex("  #ABC "), {0.7239733333333334});
    chk_opt_lum("lum:empty", _luminance_from_hex(""), std::optional<double>{});
    chk_opt_lum("lum:badhex", _luminance_from_hex("#zzzzzz"), std::optional<double>{});
    chk_opt_lum("lum:len5", _luminance_from_hex("#12345"), std::optional<double>{});
    if (failures == 0) {
        std::cout << "all cli-helper tests passed\n";
        return 0;
    }
    std::cout << failures << " cli-helper tests FAILED\n";
    return 1;
}
