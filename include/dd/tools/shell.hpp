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

// A foreground command answers within this, or it is a job: a model that blocks the turn on a ten-minute
// build learns nothing for ten minutes. Long work runs with start_job and is looked at, not waited on.
constexpr long kForegroundTimeoutMs = 120000;
// A foreground `sleep` longer than this is refused: waiting is job_call("wait"), which returns the moment
// the job ends and is bounded per call, instead of a guess at how long something takes.
constexpr long kForegroundSleepMaxS = 30;

struct ShellOptions {
    long timeout_ms = kForegroundTimeoutMs;     // wall clock for the whole process group; <= 0: none
    size_t max_output = 8000;                   // bytes handed back; the rest is read and dropped, never held
    const std::atomic<bool>* cancel = nullptr;  // set: the group is killed and the call returns
};

// {"cmd","exit","stdout"} (+ "signal", "truncated"/"output_bytes", "background_still_running"), or {"error",...}
// when refused, timed out or cancelled. "exit" is the real status: the code, or 128 + signal.
std::string run_shell(const std::string& cmd, const ShellOptions& opt = {}, const ShellPolicy& policy = {});
// Quit: SIGKILL every command group still running, background jobs included.
void shell_kill_all();

// ---- background jobs: a long command runs on while the agent works, and is looked at, not waited on ----
//
// start_job spawns `cmd` in its own process group with its output going to a log file and returns at
// once: {"job":N,"pid","log","cmd"} or {"error"}. job_call reads it back:
//   "status"  running or exit code, seconds, the output that arrived since the last look (the tail, when
//             more than max_output arrived), the total bytes and the log path for read_file
//   "wait"    the same, after the job ended or wait_s seconds passed (1..kJobWaitMaxS), whichever is first
//   "kill"    SIGTERM to the group, SIGKILL after two seconds, then the status
//   "list"    every job of this process
// Lock-free like the rest of the house: a job is a fixed slot written once by the thread that started it
// and published with a release store; the exit status is claimed by CAS, so each end is reported once.
constexpr int kJobWaitMaxS = 120;
constexpr int kJobWaitDefaultS = 30;

struct JobOptions {
    size_t max_output = 8000;                   // bytes of new output handed back per look
    const std::atomic<bool>* cancel = nullptr;  // set: a wait returns at once
};

std::string start_job(const std::string& cmd, const ShellPolicy& policy = {});
std::string job_call(const std::string& action, int job, int wait_s, const JobOptions& opt = {});
// `result` (a tool result's JSON object) with "finished_jobs" added for every job that ended since the
// model last heard of it -- each end is announced exactly once, on whichever call comes next.
std::string with_job_notices(const std::string& result);

}  // namespace tools
}  // namespace dd
