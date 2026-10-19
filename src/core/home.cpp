#include "dd/core/home.hpp"

#include <cstdlib>
#include <string>

namespace dd {
namespace core {

static std::string g_override;  // empty == unset

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\n\r");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\n\r");
    return s.substr(a, b - a + 1);
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

std::filesystem::path daedalus_home_from_env() {
    const char* v = std::getenv("DAEDALUS_HOME");
    std::string val = trim(v ? std::string(v) : std::string(""));
    if (!val.empty()) return std::filesystem::path(val);
    return platform_default_daedalus_home();
}

std::filesystem::path get_daedalus_home() {
    std::optional<std::string> ov = get_daedalus_home_override();
    if (ov.has_value() && !trim(*ov).empty()) return std::filesystem::path(trim(*ov));
    return daedalus_home_from_env();
}

}  // namespace core
}  // namespace dd
