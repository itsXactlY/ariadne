// utils.py (env/truthy subset) -> dd::core
#include "dd/core/env.hpp"

#include <cctype>
#include <cstdlib>
#include <string>

namespace dd {
namespace core {

static const char* TRUTHY_STRINGS[] = {"1", "true", "yes", "on"};

static std::string strip_lower(const std::string& s) {
    std::size_t i = 0, j = s.size();
    while (i < j && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    while (j > i && std::isspace(static_cast<unsigned char>(s[j - 1]))) --j;
    std::string out = s.substr(i, j - i);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool is_truthy_value(const std::string& value) {
    std::string t = strip_lower(value);
    for (const char* s : TRUTHY_STRINGS)
        if (t == s) return true;
    return false;
}

bool env_var_enabled(const std::string& name, const std::string& def) {
    const char* raw = std::getenv(name.c_str());
    std::string value = raw ? raw : def;
    return is_truthy_value(value);
}

}  // namespace core
}  // namespace dd
