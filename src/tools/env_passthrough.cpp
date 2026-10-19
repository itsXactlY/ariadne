// tools/env_passthrough.py -> dd::tools
#include "dd/tools/env_passthrough.hpp"

#include <cctype>
#include <filesystem>
#include <optional>
#include <set>
#include <string>

#include "dd/core/config.hpp"
#include "dd/core/home.hpp"

namespace dd {
namespace tools {

static std::set<std::string> g_allowed;  // skill-scoped allowlist
static std::optional<std::set<std::string>> g_config_passthrough;

static std::string strip(const std::string& s) {
    std::size_t i = 0, j = s.size();
    while (i < j && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    while (j > i && std::isspace(static_cast<unsigned char>(s[j - 1]))) --j;
    return s.substr(i, j - i);
}

static const std::set<std::string>& _load_config_passthrough() {
    if (g_config_passthrough) return *g_config_passthrough;
    std::set<std::string> result;
    try {
        core::Config c = core::load_config(
            (core::get_daedalus_home() / "config.yaml").string());
        const core::Json* pt = c.get("terminal.env_passthrough");
        if (pt && pt->type == core::Json::Arr) {
            for (const auto& esp : pt->arr) {
                const core::Json& item = *esp;
                if (item.type == core::Json::Str) {
                    std::string s = strip(item.str);
                    if (!s.empty()) result.insert(s);
                }
            }
        }
    } catch (...) {
        // missing/unopenable/malformed config -> empty set
    }
    g_config_passthrough = result;
    return *g_config_passthrough;
}

void register_env_passthrough(const std::vector<std::string>& var_names) {
    for (const std::string& raw : var_names) {
        std::string name = strip(raw);
        if (!name.empty()) g_allowed.insert(name);
    }
}

bool is_env_passthrough(const std::string& var_name) {
    if (g_allowed.count(var_name)) return true;
    return _load_config_passthrough().count(var_name) > 0;
}

std::set<std::string> get_all_passthrough() {
    std::set<std::string> all = g_allowed;
    const std::set<std::string>& cfg = _load_config_passthrough();
    all.insert(cfg.begin(), cfg.end());
    return all;
}

void clear_env_passthrough() { g_allowed.clear(); }
void reset_config_cache() { g_config_passthrough.reset(); }

}  // namespace tools
}  // namespace dd
