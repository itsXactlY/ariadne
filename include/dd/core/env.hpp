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
}  // namespace core
}  // namespace dd
#endif
