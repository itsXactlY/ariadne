#pragma once
// Pure subset of gateway/authz_mixin.py.
// The GatewayAuthorizationMixin method cluster (authorize_user, DM policy,
// unauthorized-DM behavior) resolves via MRO against GatewayRunner state
// (gateway.config.Platform, gateway.session.SessionSource, self.*, os.getenv,
// agent.secret_scope) and is NOT ported. Only the state-independent
// _coerce_allow_set parser is claimed.
#include "dd/core/json.hpp"
#include <string>
#include <set>

namespace dd {
namespace gateway {

// _coerce_allow_set: None -> {}; list -> {str(p).strip() for p in raw if non-empty};
// scalar -> {p.strip() for p in str(raw).split(",") if p.strip()}.
// Returned sorted/unique (models Python set).
std::set<std::string> coerce_allow_set(const core::Json& raw);

}  // namespace gateway
}  // namespace dd
