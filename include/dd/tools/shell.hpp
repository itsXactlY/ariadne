#pragma once
// dd/tools/shell.hpp — the run_terminal tool: a denylist guard and a bounded command runner.
//
// The guard is a speed bump for a model that means well, not a sandbox: it reads the command the way a shell
// would (quotes, escapes, $(...), `...`, sh -c, eval, pipelines) and refuses a short list of destructive or
// secret-touching shapes. Anything that builds a command at run time gets past it. Do not treat it as isolation.

#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

namespace dd {
namespace tools {

struct ShellPolicy {
    std::vector<std::string> extra_deny;   // config command_denylist: more command words to refuse
};

// "" when the command may run, else which rule refused it.
std::string shell_danger(const std::string& cmd, const ShellPolicy& policy = {});

struct ShellOptions {
    long timeout_ms = 600000;                   // wall clock for the whole process group; <= 0: none
    size_t max_output = 8000;                   // bytes handed back; the rest is read and dropped, never held
    const std::atomic<bool>* cancel = nullptr;  // set: the group is killed and the call returns
};

// {"cmd","exit","stdout"} (+ "signal", "truncated"/"output_bytes", "background_still_running"), or {"error",...}
// when refused, timed out or cancelled. "exit" is the real status: the code, or 128 + signal.
std::string run_shell(const std::string& cmd, const ShellOptions& opt = {}, const ShellPolicy& policy = {});
// Quit: SIGKILL every command group still running.
void shell_kill_all();

}  // namespace tools
}  // namespace dd
