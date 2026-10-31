#pragma once
// SIGTERM is a POSIX signal, so the contract names it rather than a number.
#include <csignal>
// The process-boundary meaning of a signal this process handled itself.
//
// A signal we catch is not a death: the handler records it, the loops stop, the teardown runs.
// Reporting that as the shell's 128+signo exit code afterwards is a lie at the boundary — a
// numeric 143 is a process that exited with code 143, so systemd files a deliberate stop as
// `Failed with result 'exit-code'` and, under Restart=on-failure, respawns the service that was
// just stopped on purpose.
//
// SIGTERM is a supervisor asking us to stop; an orderly teardown satisfies that request, so it
// exits 0. SIGINT and SIGHUP stay in the 128+signo convention — Ctrl-C in a terminal and a dying
// terminal are interrupts, and an interrupted run must never report a clean quit.
//
// Exit 0 is earned, not granted: a teardown that could not finish (a call that would not stop)
// keeps its own nonzero code, and callers pass 0 here only after they have joined, killed and
// freed everything they own.
namespace dd::core {

inline int exit_status_for(int sig) { return sig == SIGTERM ? 0 : 128 + sig; }

}   // namespace dd::core
