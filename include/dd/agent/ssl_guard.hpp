#pragma once
// Pure subset of agent/ssl_guard.py.
// Ported: the _skip_ssl_guard_enabled decision — given the raw env string
//   value.strip().lower() in {"1","true","yes","on"}.
// NOT ported (env / filesystem / ssl / logging coupled): _repair_hint (static
//   message), _ssl_err, _validate_bundle_path (Path/expanduser + ssl.load),
//   verify_ca_bundle / verify_ca_bundle_with_fallback (CA-bundle fs + certifi).
#include <string>

namespace dd {
namespace agent {

// true iff the raw DAEDALUS_SKIP_SSL_GUARD env value is a truthy skip word.
bool skip_ssl_guard_enabled(const std::string& raw);

}  // namespace agent
}  // namespace dd
