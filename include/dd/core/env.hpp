// utils.py (env/truthy subset) -> dd::core
#ifndef DD_CORE_ENV_HPP
#define DD_CORE_ENV_HPP
#include <cstdlib>
#include <string>
namespace dd {
namespace core {
// Shared truthy-string set from utils.py.
bool is_truthy_value(const std::string& value);
// True when env var `name` (default `def` if unset) is a truthy string.
bool env_var_enabled(const std::string& name, const std::string& def = "");
// True when `name` is set AND carries a value. An exported empty string (`DD_BASE_URL=`,
// systemd's empty assignment, a wrapper that forwards unset vars) is a var somebody set to
// nothing, not a var that carries a setting: presence alone must not read it as one. The
// product already follows this at the knobs that parse (`v && *v`); this is the same rule as
// a callable, so the gates that only test presence cannot drift from it.
bool env_present(const std::string& name);
}  // namespace core
}  // namespace dd
#endif
