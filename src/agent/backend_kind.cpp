// Pure subset of agent/backend_kind.py (see header for the porting boundary).
#include "dd/agent/backend_kind.hpp"
#include <string>
#include <optional>
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

std::optional<bool> parse_single_sequence_override(const std::string& raw) {
    std::string s = trim_lower(raw);
    if (s == "1" || s == "true" || s == "yes") return true;
    if (s == "0" || s == "false" || s == "no") return false;
    return std::nullopt;
}

}  // namespace agent
}  // namespace dd
