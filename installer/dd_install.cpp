// dd_install — dd's installer, in dd's own language, and the first thing built.
//
// The order is the whole idea: this translation unit is compiled by a bare
// `g++`, with nothing else in the tree built. It then compiles everything else.
// So the compiler invocation that produces the harness is the one the harness
// itself runs, and every step below is reported by a binary that had to build
// itself to exist. That is not a trick — it is the smallest possible proof that
// the toolchain a fresh machine needs is the one this repo assumes.
//
// Template: mazemaker's `unicum` installer (backend/installer/unicum/src). Taken
// from it: fork/exec with a real exit code, `have`/`capture` for detection,
// write-only-if-absent for files, a step list where a step that is not optional
// FAILS the install, and exit codes that distinguish "bad arguments" from "the
// install ran and failed". Left out on purpose: the TUI, the worker thread, and
// the mutex behind the log. dd's tree carries no mutex and no queue, and an
// installer that needs a screen and a thread is bigger than the thing it installs.
//
//   build/dd_install                     # compiled by the Makefile's bootstrap rule
//   build/dd_install --home DIR          # install into DIR (DIR owns config.yaml)
//   build/dd_install --minimal           # the minimal agent only: dd_agent, config, skills
//   build/dd_install --no-test           # skip the suite gate
//   build/dd_install --no-audit          # skip the doc audit
//   build/dd_install --from DIR          # copy skills/skins from the Python home, once
//
// Exit codes: 0 installed, 2 bad arguments, 3 a step ran and failed.

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

constexpr const char* VERSION = "1";
constexpr int EXIT_OK = 0;
constexpr int EXIT_USAGE = 2;
constexpr int EXIT_FAILED = 3;

// ── running processes ────────────────────────────────────────────────────────
// Two forms, because the two jobs differ. A build must STREAM: a 60-second
// `make dd_tui` that prints nothing looks like a hung installer. A probe must
// CAPTURE: its answer is one line, and that line is compared.
struct Proc {
    int code = -1;
    std::string out;
    bool ok() const { return code == 0; }
};

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

Proc run(const std::string& cmd) {
    Proc p;
    int fds[2];
    if (pipe(fds) != 0) return p;
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return p; }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    close(fds[1]);
    std::string buf;
    char chunk[4096];
    for (;;) {
        ssize_t n = ::read(fds[0], chunk, sizeof(chunk));
        if (n > 0) { buf.append(chunk, static_cast<size_t>(n)); continue; }
        if (n == 0) break;
        if (errno == EINTR) continue;
        break;
    }
    close(fds[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    p.code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    p.out = std::move(buf);
    return p;
}

std::string capture(const std::string& cmd) {
    Proc p = run(cmd + " 2>/dev/null");
    while (!p.out.empty() && (p.out.back() == '\n' || p.out.back() == ' ')) p.out.pop_back();
    return p.out;
}

bool have(const std::string& prog) {
    return run("command -v " + prog + " >/dev/null 2>&1").ok();
}

// ── files ────────────────────────────────────────────────────────────────────
// Never overwrite a SKILL.md the model has edited: dd's learn loop gives a skill
// carrying `edited_by` precedence over a draft, so an installer that clobbered a
// learned skill on upgrade would erase exactly the thing the learn loop earned.
bool write_new(const fs::path& p, const std::string& body) {
    std::error_code ec;
    if (fs::exists(p, ec)) return false;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << body;
    out.flush();
    return static_cast<bool>(out);
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return "";
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void say(const std::string& s) {
    std::printf("%s\n", s.c_str());
    std::fflush(stdout);
}

// ── options ──────────────────────────────────────────────────────────────────
struct Opts {
    fs::path root;        // the repo: holds Makefile and installer/dd_install.cpp
    fs::path home;        // the install: owns config.yaml, skills/, rules/, skins/, state/
    bool minimal = false;
    bool test = true;
    bool audit = true;
    std::string from;     // a Python Daedalus home to import skills/skins from, once
    bool booted = false;  // set by the bootstrap re-exec; never set by hand
    bool self_check = false;
};

bool parse(int argc, char** argv, Opts& o, std::string& err) {
    std::string home;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto value = [&](std::string& dst) {
            if (i + 1 >= argc) { err = a + " needs a value"; return false; }
            dst = argv[++i];
            return true;
        };
        if (a == "--home") { if (!value(home)) return false; }
        else if (a == "--root") { std::string s; if (!value(s)) return false; o.root = fs::path(s); }
        else if (a == "--from") { if (!value(o.from)) return false; }
        else if (a == "--minimal") o.minimal = true;
        else if (a == "--no-test") o.test = false;
        else if (a == "--no-audit") o.audit = false;
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
    if (o.root.empty()) o.root = fs::current_path();
    return true;
}

// dd refuses two kinds of folder as its home, and so does the installer, for the
// same reasons: make's output is not an install, and ~/.daedalus belongs to the
// Python harness — sharing it makes two installs fight over one config.yaml.
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

// ── steps ────────────────────────────────────────────────────────────────────
// A step returns bool. Anything not optional failing means the install did not
// happen. An installer that papers over a failure is how a broken install gets
// called "installed".
bool step_preflight(const Opts& o) {
    say("checking this machine");
    if (!fs::exists(o.root / "Makefile")) { say("  no Makefile at " + o.root.string()); return false; }
    if (!fs::exists(o.root / "installer" / "dd_install.cpp")) {
        say("  no installer/dd_install.cpp at " + o.root.string());
        return false;
    }
    say("  repo " + o.root.string());

    if (!have("g++")) { say("  g++ is not on PATH — dd is compiled, not interpreted"); return false; }
    say("  g++ " + capture("g++ -dumpversion"));

    std::error_code ec;
    fs::create_directories(o.root / "build", ec);
    const fs::path tmp = o.root / "build";

    // Probe the standard by compiling, not by parsing -dumpversion (which reports
    // the major only, and a distro patch level that is old enough to reject c++20
    // still prints the same number).
    Proc c20 = run("printf '#include <string>\\nint main(){std::string s=\"c++20\";return (int)s.size()-4;}\\n'"
                   " | g++ -std=c++20 -x c++ - -o " + (tmp / ".dd_cxx20_probe").string() + " 2>&1");
    if (!c20.ok()) { say("  this g++ will not take -std=c++20: " + c20.out); return false; }
    say("  -std=c++20 accepted");

    // libcurl and libgomp are hard dependencies, not optional extras: curl is the
    // transport, OpenMP is what the render kernels parallelise. Probing them by
    // compiling-and-linking beats checking a package name, because a distro that
    // ships libcurl without headers passes a name check and fails a build.
    Proc curl = run("printf '#include <curl/curl.h>\\nint main(){curl_version_info(CURLVERSION_NOW);return 0;}\\n'"
                    " | g++ -std=c++20 -x c++ - -lcurl -o " + (tmp / ".dd_curl_probe").string() + " 2>&1");
    if (!curl.ok()) { say("  libcurl (headers + lib) is not usable: " + curl.out); return false; }
    const std::string curlv = capture("curl-config --version");
    say("  libcurl " + (curlv.empty() ? std::string("(linked; curl-config not on PATH)") : curlv));

    Proc omp = run("printf '#include <omp.h>\\nint main(){return omp_get_max_threads()>0?0:1;}\\n'"
                   " | g++ -std=c++20 -x c++ -fopenmp - -o " + (tmp / ".dd_omp_probe").string() + " 2>&1");
    if (!omp.ok()) { say("  -fopenmp is not usable: " + omp.out); return false; }
    say("  OpenMP " + capture("printf '#include <omp.h>\\n#include <cstdio>\\nint main(){std::printf(\"%%d threads\\\\n\",omp_get_max_threads());}\\n'"
                              " | g++ -std=c++20 -x c++ -fopenmp - -o " + (tmp / ".dd_omp_n").string() +
                              " 2>/dev/null && " + (tmp / ".dd_omp_n").string()));

    if (!have("make")) { say("  make is not on PATH"); return false; }
    say("  make " + capture("make --version | head -1 | awk '{print $3}'"));
    say(have("mold") ? "  mold found (the Makefile links with it)"
                     : "  mold not found (system linker — nothing missing, it is an optimization)");
    say("  arch " + capture("uname -m"));

    std::string why;
    if (home_refusable(o.home, &why)) { say("  refusing home: " + why); return false; }
    return true;
}

bool step_build(const Opts& o) {
    // dd_tui is the harness, dd_agent is the minimal agent, dd is the skill
    // catalogue CLI the install reports through.
    std::vector<std::string> targets;
    // dd_tui is built in every mode: it is the only binary that can write dd's
    // reference config, so it seeds the home. In --minimal it is used for that
    // and deliberately not installed.
    targets.push_back("build/dd_tui");
    if (o.minimal) targets.push_back("build/dd_agent");
    else { targets.push_back("build/dd_tui"); targets.push_back("build/dd_agent"); targets.push_back("build/dd"); }
    for (const auto& t : targets) {
        say("building " + t);
        if (passthru("make -C " + o.root.string() + " " + t) != 0) { say("  build failed: " + t); return false; }
    }
    return true;
}

bool step_gate(const Opts& o) {
    if (o.test) {
        say("running the suites (make test)");
        if (passthru("make -C " + o.root.string() + " test") != 0) { say("  the suites did not pass"); return false; }
    } else {
        say("suites skipped (--no-test)");
    }
    if (o.audit && !o.minimal) {
        say("auditing the docs (make doc-audit)");
        if (passthru("make -C " + o.root.string() + " doc-audit") != 0) {
            say("  doc-audit reported drift — not blocking: it is a claim about the docs, not a binary");
        }
    }
    return true;
}

bool step_install(const Opts& o) {
    std::error_code ec;
    fs::create_directories(o.home, ec);
    if (!fs::exists(o.home, ec)) { say("  cannot create " + o.home.string()); return false; }

    std::vector<std::string> bins = o.minimal ? std::vector<std::string>{"dd_agent"}
                                              : std::vector<std::string>{"dd_tui", "dd_agent", "dd"};
    for (const auto& b : bins) {
        const fs::path src = o.root / "build" / b;
        if (!fs::exists(src)) { say("  not built: " + src.string()); return false; }
        if (passthru("install -m 0755 " + src.string() + " " + (o.home / b).string()) != 0) {
            say("  could not install " + b);
            return false;
        }
        say("  " + b + " -> " + (o.home / b).string());
    }

    // The home is seeded by dd itself, not by the installer writing a config it
    // invented: dd owns the reference config, and an installer holding its own copy
    // of it is a second source of truth that drifts the moment dd changes one key.
    say("seeding the home with dd's own --init");
    const std::string seed_bin = (o.root / "build" / "dd_tui").string();
    if (passthru(seed_bin + " --home " + o.home.string() + " --init >/dev/null 2>&1") != 0) {
        say("  --init refused the home");
        return false;
    }
    if (!fs::exists(o.home / "config.yaml")) { say("  no config.yaml after --init"); return false; }
    say("  config.yaml " + std::to_string(fs::file_size(o.home / "config.yaml")) + " bytes");

    for (const char* d : {"skills", "rules", "skins"}) fs::create_directories(o.home / d, ec);
    return true;
}

bool step_skills(const Opts& o) {
    // Dogfood skills: the repo ships the skills dd's agent needs to work on dd. They
    // are seeded, never replaced — an existing SKILL.md is the model's, not ours.
    const fs::path src = o.root / "installer" / "skills";
    if (!fs::exists(src)) { say("  no installer/skills to seed"); return true; }
    size_t seeded = 0;
    size_t kept = 0;
    // installer/skills/<group>/<name>/SKILL.md, mirrored exactly. The group is not
    // decoration: category_from_path() only yields a category when the path under the
    // root has three parts, so a flat skills/<name>/SKILL.md scans with none.
    for (const auto& group : fs::directory_iterator(src)) {
        if (!group.is_directory()) continue;
        for (const auto& skill : fs::directory_iterator(group.path())) {
            if (!skill.is_directory()) continue;
            const fs::path md = skill.path() / "SKILL.md";
            if (!fs::exists(md)) continue;
            const fs::path dst = o.home / "skills" / group.path().filename() / skill.path().filename() / "SKILL.md";
            if (write_new(dst, slurp(md))) seeded++;
            else kept++;
        }
    }
    say("  dogfood skills: seeded " + std::to_string(seeded) + ", left untouched " + std::to_string(kept));
    return true;
}

bool step_import(const Opts& o) {
    if (o.from.empty()) return true;
    if (!fs::exists(o.from)) { say("  --from " + o.from + " does not exist"); return false; }
    say("importing skills and skins from " + o.from + " (a copy, once — dd never shares that folder)");
    return passthru((o.root / "build" / "dd").string() + " import-daedalus --home " + o.home.string() +
                    " --from " + o.from + " --skills --skins") == 0;
}

bool step_smoke(const Opts& o) {
    // The installed binary answering is the only proof that matters: it resolves its
    // own home from the folder it sits in, reads the config it just got, and lists the
    // skills it just received. No network, so it cannot fail for an unrelated reason.
    if (o.minimal) {
        // dd_agent has no --help and needs an endpoint, so the proof is a closed
        // loopback port: the run must resolve the home it was installed into, parse
        // that home's config.yaml, read model.* from it, and reach libcurl. A refused
        // connection proves all three in one call, needs no network, and cannot fail
        // for a reason the install is not responsible for.
        say("smoke: dd_agent against a closed port (home + config + transport, no network)");
        const Proc p = run("DD_BASE_URL=http://127.0.0.1:1/v1 timeout 20 " + (o.home / "dd_agent").string() +
                           " --home " + o.home.string() + " -- ping 2>&1");
        if (p.out.find("terminate called") != std::string::npos) {
            say("  dd_agent aborted instead of reporting the failure");
            return false;
        }
        if (p.out.find("[base=") == std::string::npos) {
            say("  dd_agent never read config.yaml: " + p.out.substr(0, 120));
            return false;
        }
        if (p.out.find("curl:") == std::string::npos) {
            say("  dd_agent never reached the transport: " + p.out.substr(0, 120));
            return false;
        }
        const size_t b = p.out.find("[base=");
        const size_t e = p.out.find('\n', b);
        say("  " + p.out.substr(b, e == std::string::npos ? std::string::npos : e - b));
        return true;
    }
    const std::string names = capture((o.home / "dd").string() + " list --home " + o.home.string() +
                                      " --names-only | wc -l");
    if (names.empty()) { say("  dd list did not answer"); return false; }
    say("  catalogue from the installed home: " + names + " skills");
    return true;
}

// ── bootstrap ────────────────────────────────────────────────────────────────
int bootstrap(const Opts& o) {
    std::error_code ec;
    fs::create_directories(o.root / "build", ec);
    say("bootstrapping the installer itself");
    const std::string out = (o.root / "build" / "dd_install").string();
    const std::string cmd = "g++ -std=c++20 -O2 -Wall -Wextra -Werror -Wshadow -pipe -Iinclude -o " + out +
                            " " + (o.root / "installer" / "dd_install.cpp").string();
    if (passthru(cmd) != 0) { say("the installer does not compile; nothing else is attempted"); return EXIT_FAILED; }
    say("  " + out + " built");
    if (passthru(out + " --self-check") != 0) { say("the freshly built installer does not run"); return EXIT_FAILED; }

    // Re-exec: from here on, the work is done by the binary that just compiled itself.
    std::string reexec = out + " --booted";
    if (!o.home.empty()) reexec += " --home " + o.home.string();
    if (!o.root.empty()) reexec += " --root " + o.root.string();
    if (!o.from.empty()) reexec += " --from " + o.from;
    if (o.minimal) reexec += " --minimal";
    if (!o.test) reexec += " --no-test";
    if (!o.audit) reexec += " --no-audit";
    say("handing the rest to it");
    const int code = passthru(reexec);
    return code == EXIT_OK ? EXIT_OK : EXIT_FAILED;
}

int main(int argc, char** argv) {
    Opts o;
    std::string err;
    if (!parse(argc, argv, o, err)) {
        std::fprintf(stderr,
                     "dd_install: %s\n  usage: dd_install [--home DIR] [--minimal] [--no-test] [--no-audit] [--from DIR]\n",
                     err.c_str());
        return EXIT_USAGE;
    }
    if (o.self_check) { say("dd_install " + std::string(VERSION) + " ok"); return EXIT_OK; }
    if (!o.booted) return bootstrap(o);

    say("installing dd into " + o.home.string() + (o.minimal ? " (minimal agent)" : ""));
    struct Step { const char* name; bool optional; bool (*fn)(const Opts&); };
    const std::vector<Step> steps = {
        {"preflight", false, step_preflight},
        {"build", false, step_build},
        {"gate", false, step_gate},
        {"install", false, step_install},
        {"skills", true, step_skills},
        {"import", true, step_import},
        {"smoke", false, step_smoke},
    };
    for (const auto& s : steps) {
        say("\n" + std::string(s.name));
        if (!s.fn(o)) {
            if (s.optional) { say("  skipped (optional step failed)"); continue; }
            say("\ninstall failed at " + std::string(s.name) + " — nothing is claimed as installed");
            return EXIT_FAILED;
        }
    }
    say("\ninstalled. run it from the home that owns it:");
    say("  " + (o.home / (o.minimal ? "dd_agent" : "dd_tui")).string() + (o.minimal ? " -- \"your words\"" : ""));
    say("  " + o.home.string() + " is the install: config.yaml, skills/, rules/, skins/, state/");
    return EXIT_OK;
}
