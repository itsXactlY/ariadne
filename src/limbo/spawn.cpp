// src/limbo/spawn.cpp — impl of dd/limbo/spawn.hpp.
#include "dd/limbo/spawn.hpp"

#include "dd/core/park.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__GLIBC__)
#include <spawn.h>
#endif

extern char** environ;

namespace dd {
namespace limbo {

namespace {

constexpr std::chrono::milliseconds kWaitStep{25};

// The child's environment: ours, with the caller's overrides folded in. Held as strings so
// the pointers the child reads stay alive until execve returns.
std::vector<std::string> build_env(const std::vector<std::pair<std::string, std::string>>& env) {
    std::vector<std::string> out;
    for (char** e = environ; e && *e; ++e) out.push_back(*e);
    for (const auto& kv : env) {
        const std::string want = kv.first + "=";
        const std::string set = want + kv.second;
        bool replaced = false;
        for (auto& s : out)
            if (s.starts_with(want)) { s = set; replaced = true; break; }
        if (!replaced) out.push_back(set);
    }
    return out;
}

int open_sink(const std::string& path, std::string& error) {
    if (path.empty()) return ::open("/dev/null", O_WRONLY);
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd >= 0) return fd;
    // A log the level cannot open is not a reason to refuse a dweller: stderr still has
    // somewhere to go, and the reason is said once.
    error = "stderr log " + path + ": " + std::string(std::strerror(errno)) + "; going to /dev/null";
    return ::open("/dev/null", O_WRONLY);
}

int spawn_one(const std::string& exe, const std::vector<char*>& args, const std::string& cwd,
              const std::vector<char*>& envp, const SpawnStdio& stdio, pid_t* pid, std::string& error) {
#if defined(__GLIBC__)
    posix_spawn_file_actions_t fa;
    ::posix_spawn_file_actions_init(&fa);
    if (stdio.in_read >= 0) ::posix_spawn_file_actions_adddup2(&fa, stdio.in_read, 0);
    else ::posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    if (stdio.out_write >= 0) ::posix_spawn_file_actions_adddup2(&fa, stdio.out_write, 1);
    else ::posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    const int log_fd = stdio.log_fd >= 0 ? stdio.log_fd : ::open("/dev/null", O_WRONLY);
    if (log_fd >= 0) ::posix_spawn_file_actions_adddup2(&fa, log_fd, 2);
    ::posix_spawn_file_actions_addclosefrom_np(&fa, 3);
    // The dweller's working directory is part of its identity: a model path written relative
    // to the level's cwd would load a different file than the same command run by hand.
    if (!cwd.empty()) ::posix_spawn_file_actions_addchdir_np(&fa, cwd.c_str());
    posix_spawnattr_t at;
    ::posix_spawnattr_init(&at);
    sigset_t none, all;
    ::sigemptyset(&none);
    ::sigfillset(&all);
    ::posix_spawnattr_setsigmask(&at, &none);
    ::posix_spawnattr_setsigdefault(&at, &all);
    ::posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    const int rc = ::posix_spawn(pid, exe.c_str(), &fa, &at, args.data(), envp.data());
    ::posix_spawn_file_actions_destroy(&fa);
    ::posix_spawnattr_destroy(&at);
    if (stdio.log_fd < 0 && log_fd >= 0) ::close(log_fd);
    if (rc != 0) {
        error = "spawn " + exe + ": " + std::string(std::strerror(rc));
        return rc;
    }
    return 0;
#else
    // Not glibc (bionic, musl): neither has posix_spawn_file_actions_addclosefrom_np, so the
    // child takes posix_spawn's steps by hand and calls nothing but async-signal-safe
    // functions between fork and exec.
    const long open_max = ::sysconf(_SC_OPEN_MAX);
    const int top = open_max > 0 ? static_cast<int>(std::min<long>(open_max, 65536)) : 1024;
    const pid_t p = ::fork();
    if (p < 0) {
        error = "fork: " + std::string(std::strerror(errno));
        return errno;
    }
    if (p == 0) {
        ::setsid();
        struct sigaction dfl {};
        dfl.sa_handler = SIG_DFL;
        for (int s = 1; s < NSIG; ++s) ::sigaction(s, &dfl, nullptr);
        sigset_t mask;
        ::sigemptyset(&mask);
        ::sigprocmask(SIG_SETMASK, &mask, nullptr);
        if (stdio.in_read >= 0) ::dup2(stdio.in_read, 0);
        else {
            const int dn = ::open("/dev/null", O_RDONLY);
            if (dn > 0) { ::dup2(dn, 0); ::close(dn); }
        }
        if (stdio.out_write >= 0) ::dup2(stdio.out_write, 1);
        else {
            const int dn = ::open("/dev/null", O_WRONLY);
            if (dn > 0) { ::dup2(dn, 1); ::close(dn); }
        }
        const int log_fd = stdio.log_fd >= 0 ? stdio.log_fd : ::open("/dev/null", O_WRONLY);
        if (log_fd >= 0) ::dup2(log_fd, 2);
        for (int fd = 3; fd < top; ++fd) ::close(fd);
        if (!cwd.empty()) ::chdir(cwd.c_str());
        ::execve(exe.c_str(), args.data(), envp.data());
        ::_exit(127);
    }
    *pid = p;
    return 0;
#endif
}

// One pid's executable path, "" when /proc cannot say.
std::string exe_of(pid_t pid) {
    const std::string p = "/proc/" + std::to_string(pid) + "/exe";
    std::vector<char> buf(512);
    const ssize_t n = ::readlink(p.c_str(), buf.data(), buf.size() - 1);
    if (n <= 0) return {};
    buf[static_cast<size_t>(n)] = '\0';
    return std::string(buf.data());
}

}  // namespace

pid_t spawn_process(const std::string& exe, const std::vector<std::string>& argv, const std::string& cwd,
                    const std::vector<std::pair<std::string, std::string>>& env, const SpawnStdio& stdio,
                    std::string& error) {
    std::vector<std::string> envp_strings = build_env(env);
    std::vector<char*> envp;
    envp.reserve(envp_strings.size() + 1);
    for (auto& s : envp_strings) envp.push_back(s.data());
    envp.push_back(nullptr);

    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);

    pid_t pid = -1;
    if (spawn_one(exe, args, cwd, envp, stdio, &pid, error) != 0) return -1;
    return pid;
}

int open_log_sink(const std::string& path, std::string& error) { return open_sink(path, error); }

bool settle_exit(pid_t pid, long grace_ms, int& status) {
    // Parking on a word nobody writes is the kernel's own sleep: the grace is measured by the
    // futex timeout, not by a poll loop, and a process that exits ends the wait on the first
    // waitpid rather than after the whole grace.
    std::atomic<uint32_t> idle{0};
    for (long waited = 0; waited < grace_ms; waited += kWaitStep.count()) {
        const ssize_t seen = ::waitpid(pid, &status, WNOHANG);
        if (seen == pid) return true;
        // An error means there is no such child: somebody else already reaped it. Spending the
        // whole grace on a pid that is gone would make every death cost the grace, and a dweller
        // that is already gone is not a dweller worth waiting for.
        if (seen < 0) return false;
        core::park(idle, 0, kWaitStep);
    }
    return ::waitpid(pid, &status, WNOHANG) == pid;
}

int end_process(pid_t pid, long grace_ms, std::string& error) {
    int status = 0;
    if (pid <= 0) return -1;
    ::kill(pid, SIGTERM);
    if (settle_exit(pid, grace_ms, status)) return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &status, 0);
    error = "the dweller needed SIGKILL to end";
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void reap_leftover(const std::string& exe, pid_t keep) {
    if (exe.empty()) return;
    DIR* dir = ::opendir("/proc");
    if (!dir) return;
    std::vector<pid_t> left;
    while (struct dirent* de = ::readdir(dir)) {
        const long p = std::strtol(de->d_name, nullptr, 10);
        if (p <= 1 || p == keep || p == ::getpid()) continue;
        if (exe_of(static_cast<pid_t>(p)) == exe) left.push_back(static_cast<pid_t>(p));
    }
    ::closedir(dir);
    for (pid_t p : left) ::kill(p, SIGTERM);
    for (pid_t p : left) {
        std::string error;
        end_process(p, 5000, error);
    }
}

}  // namespace limbo
}  // namespace dd
