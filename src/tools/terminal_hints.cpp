// dd::tools::terminal_hints — output-pattern failure hints for the terminal tool.
#include "dd/tools/terminal_hints.hpp"

#include <map>
#include <optional>
#include <regex>
#include <string>

namespace dd {
namespace tools {

static const std::size_t SCAN_CHARS = 4000;

// --- individual hint detectors ---------------------------------------------

static std::optional<std::string> hint_gh_unknown_json_field(const std::string& output) {
    static const std::regex re(R"(Unknown JSON field: "?(\w+))");
    std::smatch m;
    if (!std::regex_search(output, m, re)) return std::nullopt;
    return "The installed gh does not support the JSON field '" + m[1].str() + "'. "
           "The valid field list is printed in the output above — retry using "
           "only fields from that list.";
}

static std::optional<std::string> hint_command_not_found(const std::string& output) {
    static const std::regex re(R"((?:bash: line \d+: |bash: |sh: \d*:? ?)?([\w.+-]+): command not found)");
    std::smatch m;
    if (!std::regex_search(output, m, re)) return std::nullopt;
    const std::string missing = m[1].str();
    if (missing == "python") {
        return "This system has no bare `python` — use `python3`, or the "
               "project venv's interpreter (e.g. .venv/bin/python).";
    }
    if (missing == "pip") {
        return "This system has no bare `pip` — use `pip3`, `python3 -m pip`, "
               "or the project venv's pip (e.g. .venv/bin/pip).";
    }
    return "`" + missing + "` is not installed or not on PATH. Verify with "
           "`which " + missing + "`; install it or use an absolute path instead of "
           "retrying the same command.";
}

static std::optional<std::string> hint_module_not_found(const std::string& output) {
    static const std::regex re(R"((?:ModuleNotFoundError|ImportError): No module named '?([\w.]+))");
    std::smatch m;
    if (!std::regex_search(output, m, re)) return std::nullopt;
    return "Python cannot import '" + m[1].str() + "'. Most often the wrong "
           "interpreter is running: activate the project venv (e.g. `source "
           ".venv/bin/activate`) or invoke its python directly. Only pip "
           "install if the package is genuinely absent from that venv.";
}

// re.M multiline: ^CONFLICT (line-anchored) OR "Automatic merge failed" OR "needs merge".
static std::optional<std::string> hint_merge_conflict(const std::string& output) {
    bool hit = false;
    if (output.find("Automatic merge failed") != std::string::npos) hit = true;
    if (output.find("needs merge") != std::string::npos) hit = true;
    // ^CONFLICT at any line start.
    if (output.compare(0, 9, "CONFLICT ") == 0) hit = true;
    for (std::size_t i = output.find('\n'); i != std::string::npos && !hit; i = output.find('\n', i + 1)) {
        if (output.compare(i + 1, 9, "CONFLICT ") == 0) hit = true;
    }
    if (!hit) return std::nullopt;
    return "Git merge conflict. Do not retry this command. Resolve the "
           "conflicted files listed above (edit, then `git add`), then continue "
           "(`git rebase --continue` / commit the merge) — or abort with "
           "`--abort`.";
}

static std::optional<std::string> hint_already_exists(const std::string& output) {
    static const std::regex re(R"((?:fatal|error):.*?'([^']+)' already exists)");
    std::smatch m;
    if (!std::regex_search(output, m, re)) return std::nullopt;
    return "'" + m[1].str() + "' already exists — retrying unchanged will keep "
           "failing. Reuse it, choose another name, or delete it first if it is "
           "genuinely stale.";
}

static std::optional<std::string> hint_gh_rate_limit(const std::string& output) {
    if (output.find("API rate limit") == std::string::npos &&
        output.find("was submitted too quickly") == std::string::npos)
        return std::nullopt;
    return "GitHub API rate limit hit — immediate retries will keep failing. "
           "Continue with other work and retry this operation later.";
}

static std::optional<std::string> hint_permission_denied(const std::string& output) {
    if (output.find("Permission denied") == std::string::npos &&
        output.find("EACCES") == std::string::npos)
        return std::nullopt;
    return "Permission denied. Check ownership/mode of the target path "
           "(`ls -la`); prefer a user-writable location. Only escalate to sudo "
           "if the task genuinely requires it.";
}

// --- dispatch --------------------------------------------------------------

using HintFn = std::optional<std::string> (*)(const std::string&);

static const std::map<int, std::string> EXIT_CODE_HINTS = {
    {126, "Exit 126: the file was found but is not executable — `chmod +x` it or invoke it via its interpreter (e.g. `bash script.sh`)."},
    {137, "Exit 137: the process was SIGKILLed — usually out-of-memory or an external kill. Reduce memory use or check `dmesg | tail` before retrying."},
    {124, "Exit 124: the command hit its timeout. Raise timeout= (foreground max 600s) or run it with background=true and notify_on_complete=true."},
};

std::optional<std::string> annotate_failure(const std::string& command,
                                            int exit_code,
                                            const std::string& output) {
    (void)command;  // detectors ignore command, matching the Python call sites
    if (exit_code == 0) return std::nullopt;
    std::string window = output.size() > SCAN_CHARS ? output.substr(0, SCAN_CHARS) : output;
    if (!window.empty()) {
        // Order matches _OUTPUT_HINTS.
        if (auto h = hint_gh_unknown_json_field(window)) return h;
        if (auto h = hint_merge_conflict(window)) return h;
        if (auto h = hint_command_not_found(window)) return h;
        if (auto h = hint_module_not_found(window)) return h;
        if (auto h = hint_already_exists(window)) return h;
        if (auto h = hint_gh_rate_limit(window)) return h;
        if (auto h = hint_permission_denied(window)) return h;
    }
    auto it = EXIT_CODE_HINTS.find(exit_code);
    if (it != EXIT_CODE_HINTS.end()) return it->second;
    return std::nullopt;
}

}  // namespace tools
}  // namespace dd
