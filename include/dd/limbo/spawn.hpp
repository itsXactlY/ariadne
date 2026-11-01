#pragma once
// dd/limbo/spawn.hpp — owning a dweller's process, without a pipe's assumptions.
//
// Both dwellers are processes this level owns, but they speak differently: Strata talks on two
// pipes (Charon), llama-server talks HTTP on a port. What they share is being spawned, waited
// for, and ended — so that part lives here and neither shade invents its own.
//
// A dweller gets its own session (setsid): it never reads the tty this process holds, and a
// stray child of it is endable by group without reaching this process. Its stderr goes to a
// log file, which is where a crash report is read from.
//
// Ending is a ladder, not a kill: the dweller is asked to end itself first, because freeing
// its own memory (unpinning tens of GB) takes seconds and a SIGKILL leaves the arena pinned
// until the kernel reclaims it — which is what makes the next spawn refuse to load.

#include <string>
#include <utility>
#include <vector>
#include <sys/types.h>

namespace dd {
namespace limbo {

// Which fds the child takes for its three streams. -1 means /dev/null for that stream.
struct SpawnStdio {
    int in_read = -1;      // read end of the child's stdin pipe
    int out_write = -1;    // write end of the child's stdout pipe
    int log_fd = -1;       // where stderr goes; -1 -> /dev/null
};

// argv is the whole command line, exe included — no caller adds a protocol prefix here.
// Returns the child's pid, or -1 with `error` naming what failed.
pid_t spawn_process(const std::string& exe, const std::vector<std::string>& argv, const std::string& cwd,
                   const std::vector<std::pair<std::string, std::string>>& env, const SpawnStdio& stdio,
                   std::string& error);

// Where a dweller's stderr goes: the log file, created and appended, or /dev/null when the
// level asked for no log or could not open one. The reason a log failed is said once, in
// `error`, and never turns into a refusal to spawn.
int open_log_sink(const std::string& path, std::string& error);

// Force the exit: SIGTERM, wait, SIGKILL, wait. The protocol ask (STOP + QUIT for Strata)
// happens before this, by whoever owns the pipe — a process that can be asked should be asked.
int end_process(pid_t pid, long grace_ms, std::string& error);

// Wait for a dweller to end by itself, without escalating. False means it is still alive
// after `grace_ms` — the caller then decides whether that is a death to force or a turn to
// give up on. `status` holds the exit status only when it returned true.
bool settle_exit(pid_t pid, long grace_ms, int& status);

// Processes of the same executable that this run did not adopt — an earlier run's dweller that
// was lost still holds the arena and the GPU, and the next spawn refuses to load while it
// lives. Ends them. `keep` is never touched.
void reap_leftover(const std::string& exe, pid_t keep);

}  // namespace limbo
}  // namespace dd
