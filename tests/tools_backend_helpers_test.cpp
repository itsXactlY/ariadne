// tests/tools_backend_helpers_test.cpp -> tool_backend_helpers.py
#include "dd/tools/tool_backend_helpers.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace dd::tools;
using namespace dd;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL: %s\n", #cond); } } while (0)

// Extract selected_backend as (present?string:"") + null flag.
static std::string get_selected(const core::Json& o, bool& is_null) {
    const core::Json* s = core::json_get(o, "selected_backend");
    if (!s || s->type == core::Json::Null) { is_null = true; return ""; }
    is_null = false;
    return s->type == core::Json::Str ? s->str : "";
}
static bool get_bool(const core::Json& o, const std::string& key) {
    const core::Json* b = core::json_get(o, key);
    return b && b->type == core::Json::Bool && b->b;
}
static std::string get_str(const core::Json& o, const std::string& key) {
    const core::Json* s = core::json_get(o, key);
    return (s && s->type == core::Json::Str) ? s->str : "";
}

int main() {
    // ---- managed_nous_tools_enabled ----
    setenv("DD_ENABLE_NOUS_MANAGED_TOOLS", "true", 1);
    CHECK(managed_nous_tools_enabled());
    setenv("DD_ENABLE_NOUS_MANAGED_TOOLS", "1", 1);
    CHECK(managed_nous_tools_enabled());
    setenv("DD_ENABLE_NOUS_MANAGED_TOOLS", "no", 1);
    CHECK(!managed_nous_tools_enabled());
    unsetenv("DD_ENABLE_NOUS_MANAGED_TOOLS");
    CHECK(!managed_nous_tools_enabled());

    // ---- normalize_browser_cloud_provider ----
    CHECK(normalize_browser_cloud_provider("") == "local");
    CHECK(normalize_browser_cloud_provider("  CLOUD  ") == "cloud");
    CHECK(normalize_browser_cloud_provider("  ") == "local");
    CHECK(normalize_browser_cloud_provider("Camoufox") == "camoufox");

    // ---- coerce_modal_mode / normalize_modal_mode ----
    CHECK(coerce_modal_mode("") == "auto");
    CHECK(coerce_modal_mode("DIRECT") == "direct");
    CHECK(coerce_modal_mode("  Managed ") == "managed");
    CHECK(coerce_modal_mode("bogus") == "auto");
    CHECK(normalize_modal_mode("direct") == "direct");

    // ---- has_direct_modal_credentials ----
    setenv("MODAL_TOKEN_ID", "abc", 1);
    setenv("MODAL_TOKEN_SECRET", "def", 1);
    CHECK(has_direct_modal_credentials());
    unsetenv("MODAL_TOKEN_SECRET");
    CHECK(!has_direct_modal_credentials());  // id set but secret unset
    unsetenv("MODAL_TOKEN_ID");
    // file-based: HOME/.modal.toml
    std::string h = "/tmp/dd_modal_home";
    std::error_code ec;
    std::filesystem::remove_all(h, ec);
    std::filesystem::create_directories(h);
    setenv("HOME", h.c_str(), 1);
    { std::ofstream f(h + "/.modal.toml"); f << "x"; }
    CHECK(has_direct_modal_credentials());
    std::filesystem::remove(h + "/.modal.toml");
    CHECK(!has_direct_modal_credentials());

    // ---- resolve_modal_backend_state ----
    // auto + no nous + has_direct -> direct
    {
        core::Json r = resolve_modal_backend_state("auto", true, false);
        bool nul; std::string sel = get_selected(r, nul);
        CHECK(!nul && sel == "direct");
        CHECK(get_str(r, "mode") == "auto");
        CHECK(get_bool(r, "has_direct"));
        CHECK(!get_bool(r, "managed_mode_blocked"));
    }
    // auto + nous + managed_ready -> managed
    setenv("DD_ENABLE_NOUS_MANAGED_TOOLS", "true", 1);
    {
        core::Json r = resolve_modal_backend_state("auto", true, true);
        bool nul; std::string sel = get_selected(r, nul);
        CHECK(!nul && sel == "managed");
    }
    // managed + nous off -> blocked, selected null
    unsetenv("DD_ENABLE_NOUS_MANAGED_TOOLS");
    {
        core::Json r = resolve_modal_backend_state("managed", true, true);
        bool nul; std::string sel = get_selected(r, nul);
        CHECK(nul);
        CHECK(get_bool(r, "managed_mode_blocked"));
        CHECK(get_str(r, "mode") == "managed");
    }
    // direct + no direct creds -> null
    {
        core::Json r = resolve_modal_backend_state("direct", false, true);
        bool nul; get_selected(r, nul);
        CHECK(nul);
    }

    // ---- resolve_openai_audio_api_key ----
    setenv("VOICE_TOOLS_OPENAI_KEY", "  vkey  ", 1);
    setenv("OPENAI_API_KEY", "okey", 1);
    CHECK(resolve_openai_audio_api_key() == "vkey");
    unsetenv("VOICE_TOOLS_OPENAI_KEY");
    CHECK(resolve_openai_audio_api_key() == "okey");
    unsetenv("OPENAI_API_KEY");
    CHECK(resolve_openai_audio_api_key() == "");

    if (failures == 0) {
        std::printf("all tool-backend-helpers tests passed\n");
        return 0;
    }
    std::printf("%d failures\n", failures);
    return 1;
}
