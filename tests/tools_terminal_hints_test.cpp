// Tests for dd::tools terminal_hints.
#include "dd/tools/terminal_hints.hpp"

#include <iostream>
#include <optional>
#include <string>

using namespace dd;

static int failures = 0;

static void expect(const std::string& cmd, int code, const std::string& out,
                   const std::string& want, const char* what) {
    auto got = tools::annotate_failure(cmd, code, out);
    if (!got) {
        std::cerr << "FAIL " << what << ": got=<none> want=" << want << "\n";
        ++failures;
        return;
    }
    if (*got != want) {
        std::cerr << "FAIL " << what << ": got=" << *got << " want=" << want << "\n";
        ++failures;
    }
}

static void expect_none(int code, const std::string& out, const char* what) {
    auto got = tools::annotate_failure("", code, out);
    if (got) {
        std::cerr << "FAIL " << what << ": got=" << *got << " want=<none>\n";
        ++failures;
    }
}

int main() {
    // exit 0 -> none
    expect_none(0, "anything", "exit0");

    // gh unknown json field
    expect("gh pr list --json x", 1,
           "Unknown JSON field: \"additions\"\n",
           "The installed gh does not support the JSON field 'additions'. "
           "The valid field list is printed in the output above — retry using "
           "only fields from that list.",
           "gh_json_field");

    // command not found: python special
    expect("python foo.py", 127, "bash: python: command not found\n",
           "This system has no bare `python` — use `python3`, or the "
           "project venv's interpreter (e.g. .venv/bin/python).",
           "cmdnf_python");

    // command not found: pip special
    expect("pip install x", 1, "pip: command not found\n",
           "This system has no bare `pip` — use `pip3`, `python3 -m pip`, "
           "or the project venv's pip (e.g. .venv/bin/pip).",
           "cmdnf_pip");

    // command not found: generic
    expect("foo", 1, "sh: foobar: command not found\n",
           "`foobar` is not installed or not on PATH. Verify with "
           "`which foobar`; install it or use an absolute path instead of "
           "retrying the same command.",
           "cmdnf_generic");

    // module not found
    expect("python x", 1, "ModuleNotFoundError: No module named 'requests'\n",
           "Python cannot import 'requests'. Most often the wrong "
           "interpreter is running: activate the project venv (e.g. `source "
           ".venv/bin/activate`) or invoke its python directly. Only pip "
           "install if the package is genuinely absent from that venv.",
           "module_nf");

    // merge conflict (line-anchored CONFLICT)
    expect("git merge x", 1, "On branch main\nCONFLICT (content): a.txt\n",
           "Git merge conflict. Do not retry this command. Resolve the "
           "conflicted files listed above (edit, then `git add`), then continue "
           "(`git rebase --continue` / commit the merge) — or abort with "
           "`--abort`.",
           "merge_conflict");

    // already exists
    expect("git branch new", 1, "fatal: A branch named 'new' already exists.\n",
           "'new' already exists — retrying unchanged will keep "
           "failing. Reuse it, choose another name, or delete it first if it is "
           "genuinely stale.",
           "already_exists");

    // rate limit
    expect("gh api", 1, "Error: API rate limit exceeded\n",
           "GitHub API rate limit hit — immediate retries will keep failing. "
           "Continue with other work and retry this operation later.",
           "rate_limit");

    // permission denied
    expect("cat /etc/shadow", 1, "cat: /etc/shadow: Permission denied\n",
           "Permission denied. Check ownership/mode of the target path "
           "(`ls -la`); prefer a user-writable location. Only escalate to sudo "
           "if the task genuinely requires it.",
           "perm_denied");

    // exit-code-only hints
    expect("x", 126, "no pattern here",
           "Exit 126: the file was found but is not executable — `chmod +x` it or invoke it via its interpreter (e.g. `bash script.sh`).",
           "exit126");
    expect("x", 137, "no pattern here",
           "Exit 137: the process was SIGKILLed — usually out-of-memory or an external kill. Reduce memory use or check `dmesg | tail` before retrying.",
           "exit137");
    expect("x", 124, "no pattern here",
           "Exit 124: the command hit its timeout. Raise timeout= (foreground max 600s) or run it with background=true and notify_on_complete=true.",
           "exit124");

    // no hint at all (exit 7, no pattern)
    expect_none(7, "totally unrelated output", "no_hint");

    if (failures == 0) {
        std::cout << "all terminal-hints tests passed\n";
        return 0;
    }
    std::cout << failures << " terminal-hints test(s) failed\n";
    return 1;
}
