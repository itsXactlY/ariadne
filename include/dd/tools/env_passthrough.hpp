// tools/env_passthrough.py -> dd::tools
#ifndef DD_TOOLS_ENV_PASSTHROUGH_HPP
#define DD_TOOLS_ENV_PASSTHROUGH_HPP
#include <set>
#include <string>
#include <vector>
namespace dd {
namespace tools {
// Register env-var names as allowed in sandboxed environments (skill-declared).
void register_env_passthrough(const std::vector<std::string>& var_names);
// True if var_name was skill-registered or is in config tools.env_passthrough.
bool is_env_passthrough(const std::string& var_name);
// Union of skill-registered and config-based passthrough vars.
std::set<std::string> get_all_passthrough();
// Reset the skill-scoped allowlist (session reset).
void clear_env_passthrough();
// Force re-read of config on next access (testing).
void reset_config_cache();
}  // namespace tools
}  // namespace dd
#endif
