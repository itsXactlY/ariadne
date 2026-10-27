// dd_install — a sidegag, not an installer that brings everything.
//
// It does one thing worth writing down: it compiles itself, then compiles the harness.
// This TU is built by a bare `g++` with nothing else in the tree built, then re-execs
// itself, so the compiler invocation that produces dd is the one dd's own build runs.
//
// Deliberately absent: toolchain probes, a suite gate, a doc audit, skill seeding, an
// import step, a captured-output pipe. If libcurl or OpenMP is missing `make` says so in
// the language of the failure that actually matters; a probe that repeats that in advance
// is a line the installer would only paraphrase. Refusing two bad homes stays, because
// that is an invariant `make` does not check, not a report.
//
//   build/dd_install                            # into $HOME/projects/daedalus
//   build/dd_install --home DIR                 # into DIR — DIR owns config.yaml
//   build/dd_install --home DIR --agent --cli   # also dd_agent, also dd
//
// Exit codes: 0 installed, 2 bad arguments, 3 build or install failed.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

constexpr const char* VERSION = "2";
constexpr int EXIT_OK = 0;
constexpr int EXIT_USAGE = 2;
constexpr int EXIT_FAILED = 3;

// stdio inherited, so a compile reports as it happens. Nothing is captured: there is no
// probe left here whose answer would be compared.
int passthru(const std::string& cmd) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

void say(const std::string& s) {
    std::printf("%s\n", s.c_str());
    std::fflush(stdout);
}

struct Opts {
    fs::path root;      // the repo: holds Makefile and installer/dd_install.cpp
    fs::path home;      // the install: owns config.yaml, state/, logs/
    bool agent = false; // also install dd_agent
    bool cli = false;   // also install dd (the skill catalogue CLI)
    bool booted = false;
    bool self_check = false;
};

bool parse(int argc, char** argv, Opts& o, std::string& err) {
    std::string home, root;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto value = [&](std::string& dst) {
            if (i + 1 >= argc) { err = a + " needs a value"; return false; }
            dst = argv[++i];
            return true;
        };
        if (a == "--home") { if (!value(home)) return false; }
        else if (a == "--root") { if (!value(root)) return false; }
        else if (a == "--agent") o.agent = true;
        else if (a == "--cli") o.cli = true;
        else if (a == "--booted") o.booted = true;
        else if (a == "--self-check") o.self_check = true;
        else if (a == "--help" || a == "-h") { o.self_check = true; return true; }
        else { err = "unknown argument: " + a; return false; }
    }
    if (home.empty()) {
        const char* h = std::getenv("HOME");
        if (!h || *h == '\0') { err = "--home DIR, or HOME set"; return false; }
        home = std::string(h) + "/projects/daedalus";
    }
    o.home = fs::path(home);
    o.root = root.empty() ? fs::current_path() : fs::path(root);
    return true;
}

// dd refuses two kinds of folder as its home and the sidegag mirrors it, because `make`
// never checks it: make's output is not an install, and ~/.daedalus belongs to the Python
// harness — sharing it makes two installs fight over one config.yaml.
bool home_refusable(const fs::path& home, std::string* why) {
    const std::string leaf = home.filename().string();
    if (leaf == "build" || leaf == "out" || leaf == "obj") {
        if (why) *why = "a folder named '" + leaf + "' is make's output, not an install";
        return true;
    }
    const char* h = std::getenv("HOME");
    if (h && home == fs::path(h) / ".daedalus") {
        if (why) *why = "~/.daedalus belongs to the Python Daedalus; dd lives somewhere else";
        return true;
    }
    return false;
}

int bootstrap(const Opts& o) {
    std::error_code ec;
    fs::create_directories(o.root / "build", ec);
    say("compiling the sidegag itself");
    const std::string out = (o.root / "build" / "dd_install").string();
    const std::string cmd = "g++ -std=c++20 -O2 -Wall -Wextra -Werror -Wshadow -pipe -Iinclude -o " + out +
                            " " + (o.root / "installer" / "dd_install.cpp").string();
    if (passthru(cmd) != 0) { say("it does not compile; nothing else is attempted"); return EXIT_FAILED; }
    if (passthru(out + " --self-check") != 0) { say("the freshly built binary does not run"); return EXIT_FAILED; }

    std::string re = out + " --booted --root " + o.root.string() + " --home " + o.home.string();
    if (o.agent) re += " --agent";
    if (o.cli) re += " --cli";
    say("handing the rest to it");
    return passthru(re) == EXIT_OK ? EXIT_OK : EXIT_FAILED;
}

int main(int argc, char** argv) {
    Opts o;
    std::string err;
    if (!parse(argc, argv, o, err)) {
        std::fprintf(stderr, "dd_install: %s\n  usage: dd_install [--home DIR] [--agent] [--cli]\n", err.c_str());
        return EXIT_USAGE;
    }
    if (o.self_check) { say("dd_install " + std::string(VERSION) + " ok"); return EXIT_OK; }
    if (!o.booted) return bootstrap(o);

    std::string why;
    if (home_refusable(o.home, &why)) { say("refusing home: " + why); return EXIT_FAILED; }
    if (!fs::exists(o.root / "Makefile")) { say("no Makefile at " + o.root.string()); return EXIT_FAILED; }

    // The folder has to exist before anything can be installed into it. dd's --init may
    // create a home, but it runs after the copy, so an absent DIR fails the copy first —
    // with a message about a file that cannot be created, which is not the real cause.
    std::error_code hc;
    fs::create_directories(o.home, hc);
    if (!fs::exists(o.home, hc)) { say("cannot create " + o.home.string()); return EXIT_FAILED; }

    // dd_tui is always built and installed: it is the harness, and it is the only binary
    // that can write dd's reference config. dd's own --init seeds the home — dd owns that
    // config, so an installer holding a second copy of it would drift the moment dd
    // changes one key.
    std::vector<std::string> targets{"dd_tui"};
    if (o.agent) targets.push_back("dd_agent");
    if (o.cli) targets.push_back("dd");

    const std::string root = o.root.string();
    for (const auto& t : targets) {
        say("make build/" + t);
        if (passthru("make -C " + root + " build/" + t) != 0) { say("build failed: " + t); return EXIT_FAILED; }
        if (passthru("install -m 0755 " + (o.root / "build" / t).string() + " " + (o.home / t).string()) != 0) {
            say("could not install " + t);
            return EXIT_FAILED;
        }
        say("  " + t + " -> " + (o.home / t).string());
    }

    say("seeding the home with dd's own --init");
    if (passthru((o.root / "build" / "dd_tui").string() + " --home " + o.home.string() + " --init >/dev/null 2>&1") != 0) {
        say("--init refused the home");
        return EXIT_FAILED;
    }
    std::error_code ec;
    if (!fs::exists(o.home / "config.yaml", ec)) { say("no config.yaml after --init"); return EXIT_FAILED; }

    // The only proof this claims: the binary that landed runs.
    if (passthru((o.home / "dd_tui").string() + " --version") != 0) { say("the installed dd_tui does not run"); return EXIT_FAILED; }
    say("installed: " + o.home.string() + " (config.yaml " + std::to_string(fs::file_size(o.home / "config.yaml", ec)) +
        " bytes)");
    say("run it there: " + (o.home / "dd_tui").string());
    return EXIT_OK;
}
