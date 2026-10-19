// tools/tool_backend_helpers.py -> dd::tools
#include "dd/tools/tool_backend_helpers.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "dd/core/env.hpp"
#include "dd/core/json.hpp"

namespace dd {
namespace tools {

static std::string strip_lower(const std::string& s) {
    std::size_t i = 0, j = s.size();
    while (i < j && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    while (j > i && std::isspace(static_cast<unsigned char>(s[j - 1]))) --j;
    std::string out = s.substr(i, j - i);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool managed_nous_tools_enabled() {
    return core::env_var_enabled("DAEDALUS_ENABLE_NOUS_MANAGED_TOOLS");
}

std::string normalize_browser_cloud_provider(const std::string& value) {
    std::string provider = strip_lower(value.empty() ? "local" : value);
    return provider.empty() ? "local" : provider;
}

std::string coerce_modal_mode(const std::string& value) {
    std::string mode = strip_lower(value.empty() ? "auto" : value);
    if (mode == "auto" || mode == "direct" || mode == "managed") return mode;
    return "auto";
}

std::string normalize_modal_mode(const std::string& value) { return coerce_modal_mode(value); }

bool has_direct_modal_credentials() {
    const char* id = std::getenv("MODAL_TOKEN_ID");
    const char* secret = std::getenv("MODAL_TOKEN_SECRET");
    bool env_ok = (id && *id) && (secret && *secret);
    bool file_ok = false;
    const char* home = std::getenv("HOME");
    if (home && *home) {
        std::error_code ec;
        file_ok = std::filesystem::exists(std::filesystem::path(home) / ".modal.toml", ec);
    }
    return env_ok || file_ok;
}

core::Json resolve_modal_backend_state(const std::string& modal_mode, bool has_direct, bool managed_ready) {
    std::string requested_mode = coerce_modal_mode(modal_mode);
    std::string normalized_mode = normalize_modal_mode(modal_mode);
    bool nous = managed_nous_tools_enabled();
    bool managed_mode_blocked = (requested_mode == "managed" && !nous);

    core::Json selected;  // null by default
    if (normalized_mode == "managed") {
        if (nous && managed_ready) selected = core::json_str("managed");
    } else if (normalized_mode == "direct") {
        if (has_direct) selected = core::json_str("direct");
    } else {  // auto
        if (nous && managed_ready)
            selected = core::json_str("managed");
        else if (has_direct)
            selected = core::json_str("direct");
    }

    core::Json out = core::json_obj();
    core::obj_set(out, "requested_mode", core::json_str(requested_mode));
    core::obj_set(out, "mode", core::json_str(normalized_mode));
    core::obj_set(out, "has_direct", core::json_bool(has_direct));
    core::obj_set(out, "managed_ready", core::json_bool(managed_ready));
    core::obj_set(out, "managed_mode_blocked", core::json_bool(managed_mode_blocked));
    core::obj_set(out, "selected_backend", selected);
    return out;
}

std::string resolve_openai_audio_api_key() {
    const char* voice = std::getenv("VOICE_TOOLS_OPENAI_KEY");
    std::string v = voice ? voice : "";
    if (v.empty()) {
        const char* oa = std::getenv("OPENAI_API_KEY");
        v = oa ? oa : "";
    }
    // .strip()
    std::size_t i = 0, j = v.size();
    while (i < j && std::isspace(static_cast<unsigned char>(v[i]))) ++i;
    while (j > i && std::isspace(static_cast<unsigned char>(v[j - 1]))) --j;
    return v.substr(i, j - i);
}

}  // namespace tools
}  // namespace dd
