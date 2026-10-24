#include "dd/core/home.hpp"

#include <cctype>
#include <cstdlib>
#include <string>
#include <system_error>

namespace dd {
namespace core {

static std::string g_override;  // empty == unset

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\n\r");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\n\r");
    return s.substr(a, b - a + 1);
}

static std::string env_str(const char* key) {
    const char* v = std::getenv(key);
    return trim(v ? std::string(v) : std::string());
}

void set_daedalus_home_override(const std::string& path) { g_override = path; }

std::optional<std::string> get_daedalus_home_override() {
    if (g_override.empty()) return std::nullopt;
    return g_override;
}

std::filesystem::path platform_default_daedalus_home() {
    const char* h = std::getenv("HOME");
    std::string home = (h && *h) ? std::string(h) : std::string("/");
    return std::filesystem::path(home) / ".daedalus";
}

std::filesystem::path exe_dir() {
    std::error_code ec;
    const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec || exe.empty()) return {};
    return exe.parent_path();
}

// A folder that already carries its own config.yaml: that config governs.
static bool has_own_config(const std::filesystem::path& dir) {
    std::error_code ec;
    return std::filesystem::exists(dir / "config.yaml", ec);
}

// A folder already in use as a dd home: it has working folders under it.
static bool has_own_state(const std::filesystem::path& dir) {
    for (const char* sub : {"state", "cache", "logs", "learn"}) {
        std::error_code ec;
        if (std::filesystem::exists(dir / sub, ec)) return true;
    }
    return false;
}

// make's output folder: `build/` holds what the Makefile produced, plus whatever
// an accidental start left in it. It is never an install — the install is the
// folder the binary is copied to and run from, the one that owns a config.yaml.
static bool is_build_output(const std::filesystem::path& dir) {
    std::string name = dir.filename().string();
    for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return name == "build" || name == "out" || name == "obj";
}

// $HOME/.daedalus — the Python install's folder. dd never lives there: sharing
// that folder lets the two fight over config.yaml, state/, skills and skins.
static bool is_python_home(const std::filesystem::path& p) {
    if (p.empty()) return false;
    std::error_code a, b;
    return std::filesystem::weakly_canonical(p, a) ==
           std::filesystem::weakly_canonical(platform_default_daedalus_home(), b);
}

std::filesystem::path daedalus_home_from_env() {
    for (const char* key : {"DD_HOME", "DAEDALUS_HOME"})
        if (const std::string v = env_str(key); !v.empty()) return std::filesystem::path(v);

    const auto exe = exe_dir();
    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);

    // Installed: the folder the binary sits in already carries a config.yaml.
    if (!exe.empty() && has_own_config(exe)) return exe;
    // Started from the project folder: its config.yaml governs.
    if (!cwd.empty() && has_own_config(cwd)) return cwd;
    // A folder already in use as a home: it has working folders under it.
    if (!exe.empty() && has_own_state(exe)) return exe;
    if (!cwd.empty() && has_own_state(cwd)) return cwd;
    // Neither owns anything: work from where dd was started.
    if (!cwd.empty()) return cwd;
    return exe;  // no /proc and no cwd: the binary's folder decides
}

bool is_python_daedalus_home(const std::filesystem::path& p) { return is_python_home(p); }

std::filesystem::path get_daedalus_home() {
    std::optional<std::string> ov = get_daedalus_home_override();
    if (ov.has_value() && !trim(*ov).empty()) return std::filesystem::path(trim(*ov));
    return daedalus_home_from_env();
}

Home resolve_home(const std::string& home_flag, const std::string& config_flag, std::string* why) {
    namespace fs = std::filesystem;
    const std::string hf = trim(home_flag), cf = trim(config_flag);
    std::string asked = "--home";
    if (hf.empty() && !cf.empty()) asked = "--config";
    if (hf.empty() && cf.empty()) asked = "DD_HOME / DAEDALUS_HOME";

    Home h;
    if (!hf.empty()) {
        h.root = hf;
    } else if (!cf.empty()) {
        h.root = fs::path(cf).parent_path();
        if (h.root.empty()) h.root = ".";
    } else {
        h.root = daedalus_home_from_env();
    }
    std::error_code ec;
    const fs::path abs = fs::absolute(h.root, ec);
    if (!ec) h.root = abs.lexically_normal();

    // Asking for the Python install's home is the one mix that breaks: an
    // empty root says "no home", and *why says what to do instead.
    if (is_python_home(h.root)) {
        if (why)
            *why = "dd will not run from " + h.root.string() + ": that is the Python install's home, and "
                   "sharing it lets the two fight over config.yaml, state/, skills and skins. dd keeps its "
                   "own folder — pass --home somewhere else, or DD_HOME=somewhere else. What you want from "
                   "the other install: dd import-daedalus --skills --skins --config --from " +
                   h.root.string() + ", which copies once. (asked by " + asked + ")";
        return Home{};
    }
    // Picked by the folder rules, not asked for: a folder named build/ is make's
    // output, and dd will not turn it into an install. The install owns a config.
    if (hf.empty() && cf.empty() && is_build_output(h.root) && !has_own_config(h.root)) {
        if (why)
            *why = "dd found only " + h.root.string() + ", which is make's output folder, not an install: "
                   "dd will not create a home there. Copy the built binary to the folder that owns the "
                   "config.yaml (the install) and run it from there, or name that folder with --home DIR.";
        return Home{};
    }
    // No binary folder and no launch folder: nothing is a home, and dd says so
    // rather than inventing one under $HOME.
    if (h.root.empty() && why) *why = "dd found no folder to work from: pass --home DIR or DD_HOME=DIR";
    return h;
}

bool ensure_home(const Home& h, std::string* why) {
    std::error_code ec;
    for (const auto& dir : {h.root, h.state(), h.cache(), h.logs()}) {
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            if (why) *why = "cannot create " + dir.string() + ": " + ec.message();
            return false;
        }
    }
    return true;
}

}  // namespace core
}  // namespace dd
