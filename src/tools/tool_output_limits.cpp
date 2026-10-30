// tools/tool_output_limits.py -> dd::tools
#include "dd/tools/tool_output_limits.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>
#include <string>

#include "dd/core/config.hpp"
#include "dd/core/home.hpp"

namespace dd {
namespace tools {

// int(value) or default on any issue; positive-only.
int _coerce_positive_int(const core::Json* value, int def) {
    if (!value) return def;
    long long iv = 0;
    switch (value->type) {
        case core::Json::Num:
            iv = static_cast<long long>(value->num);  // truncates toward zero
            break;
        case core::Json::Bool:
            iv = value->b ? 1 : 0;
            break;
        case core::Json::Str: {
            std::string s = value->str;
            std::size_t i = 0, j = s.size();
            while (i < j && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
            while (j > i && std::isspace(static_cast<unsigned char>(s[j - 1]))) --j;
            std::string t = s.substr(i, j - i);
            std::size_t k = 0;
            if (k < t.size() && (t[k] == '+' || t[k] == '-')) ++k;
            if (k >= t.size()) return def;  // empty or lone sign
            for (; k < t.size(); ++k)
                if (!std::isdigit(static_cast<unsigned char>(t[k]))) return def;
            try {
                iv = std::stoll(t);
            } catch (...) {
                return def;
            }
            break;
        }
        default:
            return def;  // Null / Arr / Obj -> TypeError
    }
    if (iv <= 0) return def;
    return static_cast<int>(iv);
}

// Process-lifetime cache of resolved limits (nullopt = not yet read).
static std::optional<std::map<std::string, int>> g_limits_cache;

const std::map<std::string, int>& get_tool_output_limits() {
    if (g_limits_cache) return *g_limits_cache;

    const core::Json* mb = nullptr, *ml = nullptr, *mll = nullptr;
    try {
        core::Config c = core::load_config(
            (core::get_ariadne_home() / "config.yaml").string());
        mb = c.get("tool_output.max_bytes");
        ml = c.get("tool_output.max_lines");
        mll = c.get("tool_output.max_line_length");
    } catch (...) {
        // missing/unopenable/malformed config -> all defaults
    }

    std::map<std::string, int> limits;
    limits["max_bytes"] = _coerce_positive_int(mb, DEFAULT_MAX_BYTES);
    limits["max_lines"] = _coerce_positive_int(ml, DEFAULT_MAX_LINES);
    limits["max_line_length"] = _coerce_positive_int(mll, DEFAULT_MAX_LINE_LENGTH);
    g_limits_cache = limits;
    return *g_limits_cache;
}

std::map<std::string, int> resolve_tool_output_from(const core::Config& cfg) {
    std::map<std::string, int> env;
    env["max_bytes"] = _coerce_positive_int(cfg.get("tool_output.max_bytes"), SHIPPED_MAX_BYTES);
    env["max_lines"] = _coerce_positive_int(cfg.get("tool_output.max_lines"), SHIPPED_MAX_LINES);
    env["max_line_length"] = _coerce_positive_int(cfg.get("tool_output.max_line_length"), SHIPPED_MAX_LINE_LENGTH);
    env["read_bytes"] = _coerce_positive_int(cfg.get("tool_output.read_bytes"), SHIPPED_READ_BYTES);
    return env;
}

void _reset_tool_output_limits_cache() { g_limits_cache.reset(); }

int get_max_bytes() { return get_tool_output_limits().at("max_bytes"); }
int get_max_lines() { return get_tool_output_limits().at("max_lines"); }
int get_max_line_length() { return get_tool_output_limits().at("max_line_length"); }

}  // namespace tools
}  // namespace dd
