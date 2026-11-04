#pragma once
// dd/agent/attribution.hpp — the one place a run reads its own identity out of the config.
//
// Both binaries (the screen and ariadne-agent) ask this, so a fix here is a fix in both: an
// identity resolved twice is an identity that drifts. The mechanism and why it exists are in
// dd/agent/http_llm.hpp (Attribution); this header is only the config/env half:
//
//   model.referer        the app's URL: the identifier a provider files usage under
//   model.app_title      the display name in the rankings
//   model.app_categories comma-separated, only names the provider documents
//   model.attribution    off | none | false -> send no header at all
//
// DD_REFERER / DD_APP_TITLE / DD_APP_CATEGORIES win over the file, as every other DD_ knob does.

#include "dd/agent/http_llm.hpp"
#include "dd/core/config.hpp"

#include <cstdlib>
#include <string>

namespace dd {
namespace agent {

inline std::string attribution_env(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

inline Attribution attribution_from_config(const core::Config& cfg, const std::string& base_url) {
    if (attribution_off(cfg.get("model.attribution"))) return {};
    Attribution configured;
    configured.referer = attribution_env("DD_REFERER", cfg.get_string("model.referer"));
    configured.title = attribution_env("DD_APP_TITLE", cfg.get_string("model.app_title"));
    configured.categories = attribution_env("DD_APP_CATEGORIES", cfg.get_string("model.app_categories"));
    return attribution_for(base_url, configured);
}

}  // namespace agent
}  // namespace dd
