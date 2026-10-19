// Differential test for the pure root-file helpers: run_agent _is_destructive_command
// + _paths_overlap, cli _looks_like_slash_command (vs captured Python ground truth).
#include <iostream>
#include <string>
#include <vector>

#include "dd/agent/run_helpers.hpp"
#include "dd/cli/heuristics.hpp"

using namespace dd;
using namespace dd::agent;
using namespace dd::cli;

static int failures = 0;
static void chk(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::cout << "FAIL " << what << "\n";
    }
}

int main() {
    // ---- _is_destructive_command ----
    chk(!_is_destructive_command(""), "dest:empty");
    chk(!_is_destructive_command("ls"), "dest:ls");
    chk(_is_destructive_command("rm -rf /"), "dest:rm-rf");
    chk(_is_destructive_command("sudo rm x"), "dest:sudo-rm");
    chk(_is_destructive_command("git clean -fdx"), "dest:git-clean");
    chk(!_is_destructive_command("git checkout"), "dest:git-checkout-nospace");
    chk(_is_destructive_command("echo a > b"), "dest:redir-single");
    chk(!_is_destructive_command("echo a >> b"), "dest:redir-double");
    chk(_is_destructive_command("echo a>b"), "dest:redir-nospace");
    chk(!_is_destructive_command("echo a|b"), "dest:pipe");
    chk(_is_destructive_command("truncate -s 0 f"), "dest:truncate");
    chk(_is_destructive_command("dd if=x"), "dest:dd");
    chk(_is_destructive_command("mv a b"), "dest:mv");
    chk(_is_destructive_command("sed -i s/a/b/ f"), "dest:sed-i");
    chk(!_is_destructive_command("rm"), "dest:rm-nospace");
    chk(!_is_destructive_command("git reset"), "dest:git-reset-nospace");

    // ---- _paths_overlap ----
    using V = std::vector<std::string>;
    chk(!_paths_overlap(V{"a", "b"}, V{"a", "c"}), "overlap:diverge");
    chk(_paths_overlap(V{"a"}, V{"a", "b"}), "overlap:prefix");
    chk(!_paths_overlap({}, {}), "overlap:both-empty");
    chk(!_paths_overlap(V{"a"}, {}), "overlap:right-empty");
    chk(!_paths_overlap({}, V{"a"}), "overlap:left-empty");
    chk(_paths_overlap(V{"a", "b", "c"}, V{"a", "b"}), "overlap:right-prefix");
    chk(!_paths_overlap(V{"/", "usr", "x"}, V{"/", "usr", "y"}), "overlap:fs-diverge");

    // ---- _looks_like_slash_command ----
    chk(_looks_like_slash_command("/help"), "slash:help");
    chk(_looks_like_slash_command("/model gpt-4"), "slash:model");
    chk(!_looks_like_slash_command("/Users/ironin/file.md:45-46 can you fix this?"), "slash:pasted-path");
    chk(!_looks_like_slash_command(""), "slash:empty");
    chk(!_looks_like_slash_command("hello"), "slash:not-slash");
    chk(_looks_like_slash_command("/a"), "slash:short");
    chk(_looks_like_slash_command("/q"), "slash:q");
    chk(!_looks_like_slash_command("//x"), "slash:double-slash");
    chk(_looks_like_slash_command("/"), "slash:bare-slash");

    if (failures == 0) {
        std::cout << "all root helper tests passed\n";
        return 0;
    }
    std::cout << failures << " root helper tests FAILED\n";
    return 1;
}
