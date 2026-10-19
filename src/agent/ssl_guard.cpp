// Pure subset of agent/ssl_guard.py (see header for the porting boundary).
#include "dd/agent/ssl_guard.hpp"
#include <string>
#include <cctype>

namespace dd {
namespace agent {

static std::string trim_lower(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    std::string out = s.substr(a, b - a);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool skip_ssl_guard_enabled(const std::string& raw) {
    std::string s = trim_lower(raw);
    return s == "1" || s == "true" || s == "yes" || s == "on";
}

}  // namespace agent
}  // namespace dd
