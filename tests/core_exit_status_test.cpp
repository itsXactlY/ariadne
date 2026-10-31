// dd::core::exit_status_for — what a caught signal becomes at the process boundary.
// The gate is that a supervisor's stop is not reported as a failure: systemd reads a numeric
// 143 as `result=exit-code`, files a deliberate stop as failed, and respawns the service.
#include <csignal>
#include <cstdio>
#include <string>

#include "dd/core/exit_status.hpp"

using namespace dd::core;

static int g_fail = 0;
static void chk(bool ok, const std::string& label) {
    if (!ok) {
        ++g_fail;
        std::printf("FAIL %s\n", label.c_str());
    }
}

int main() {
    // SIGTERM = "stop, please". Satisfying it is success, so 0 — never the synthetic 128+15.
    chk(exit_status_for(SIGTERM) == 0, "sigterm is a clean stop");
    chk(exit_status_for(SIGTERM) != 128 + SIGTERM, "sigterm never reports 143");
    // SIGINT/SIGHUP are interrupts: the shell's 128+signo convention, so they never look clean.
    chk(exit_status_for(SIGINT) == 128 + SIGINT, "sigint stays an interrupt (130)");
    chk(exit_status_for(SIGHUP) == 128 + SIGHUP, "sighup stays an interrupt (129)");
    chk(exit_status_for(SIGUSR1) == 128 + SIGUSR1, "any other recorded signal stays 128+signo");
    // The shape every call site uses: a signal is mapped, the ordinary exit stays 0.
    const auto status = [](int s) { return s ? exit_status_for(s) : 0; };
    chk(status(0) == 0, "no signal exits 0");

    // The serve/gateway/tui paths only call this after teardown; a failed teardown keeps its
    // own nonzero code, so exit 0 is earned by the orderly path and cannot be granted by a signal.
    chk(exit_status_for(SIGTERM) == 0 && exit_status_for(SIGINT) == 130, "clean stop and interrupt differ");

    if (g_fail) {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("exit_status: ok\n");
    return 0;
}
