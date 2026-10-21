#include "dd/core/home.hpp"

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

std::filesystem::path daedalus_home_from_env() {
    for (const char* key : {"DD_HOME", "DAEDALUS_HOME"})
        if (const std::string v = env_str(key); !v.empty()) return std::filesystem::path(v);
    if (std::filesystem::path d = exe_dir(); !d.empty()) return d;
    std::error_code ec;
    return std::filesystem::current_path(ec);   // no /proc: beside where we were started
}

std::filesystem::path get_daedalus_home() {
    std::optional<std::string> ov = get_daedalus_home_override();
    if (ov.has_value() && !trim(*ov).empty()) return std::filesystem::path(trim(*ov));
    return daedalus_home_from_env();
}

Home resolve_home(const std::string& home_flag, const std::string& config_flag) {
    namespace fs = std::filesystem;
    Home h;
    if (!trim(home_flag).empty()) {
        h.root = trim(home_flag);
    } else if (!trim(config_flag).empty()) {
        h.root = fs::path(trim(config_flag)).parent_path();
        if (h.root.empty()) h.root = ".";
    } else {
        h.root = daedalus_home_from_env();
    }
    std::error_code ec;
    const fs::path abs = fs::absolute(h.root, ec);
    if (!ec) h.root = abs.lexically_normal();
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
