// tools/tool_backend_helpers.py -> dd::tools
#ifndef DD_TOOLS_TOOL_BACKEND_HELPERS_HPP
#define DD_TOOLS_TOOL_BACKEND_HELPERS_HPP
#include <string>
#include "dd/core/json.hpp"
namespace dd {
namespace tools {
// Hidden Nous-managed tools feature flag.
bool managed_nous_tools_enabled();
// Normalized browser provider key (default "local").
std::string normalize_browser_cloud_provider(const std::string& value);
// Requested modal mode when valid, else default "auto".
std::string coerce_modal_mode(const std::string& value);
std::string normalize_modal_mode(const std::string& value);
// True when direct Modal credentials/config are available.
bool has_direct_modal_credentials();
// Resolve direct vs managed backend selection (returns a JSON object).
core::Json resolve_modal_backend_state(const std::string& modal_mode, bool has_direct, bool managed_ready);
// Voice-tools OpenAI key with fallback to the normal key.
std::string resolve_openai_audio_api_key();
}  // namespace tools
}  // namespace dd
#endif
