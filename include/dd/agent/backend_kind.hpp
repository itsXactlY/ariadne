#pragma once
// Pure subset of agent/backend_kind.py.
// Ported: the DAEDALUS_SINGLE_SEQUENCE env-override tri-state parser
// (raw.strip().lower() then membership in the truthy/falsy word sets).
// NOT ported (env / http / time-cache coupled): is_single_sequence's
// server_root lookup, /metrics urllib probe, TTL cache, time.monotonic.
#include <string>
#include <optional>

namespace dd {
namespace agent {

// raw env value -> tri-state override (nullopt = no override).
std::optional<bool> parse_single_sequence_override(const std::string& raw);

}  // namespace agent
}  // namespace dd
