#pragma once
#include <optional>
#include <string>
#include "dd/core/json.hpp"

namespace dd {
namespace cron {

// Faithful port of cron/jobs.py scheduling core (parse_schedule, compute_next_run,
// _compute_grace_seconds, _recoverable_oneshot_run_at, parse_duration) plus a
// croniter-equivalent 5-field matcher (cron_next).
//
// Datetime model: minute-granularity proleptic-Gregorian minutes-since-epoch.
// Python's tz-aware local time is simplified to a single fixed offset (UTC);
// scheduling arithmetic is offset-invariant, so this preserves ordering and
// next-run computation. DST wall-clock edges are out of scope.

struct DT {
    long long minute;  // minutes since epoch (UTC)
};

// ISO-8601 <-> DT. Accepts "YYYY-MM-DDTHH:MM[:SS][Z|+HH:MM]"; naive treated as UTC.
DT parse_iso(const std::string& iso);  // throws std::runtime_error on malformed
std::string iso_of(const DT& dt);       // "YYYY-MM-DDTHH:MM:SS+00:00"

// Current wall-clock time as DT (minutes since epoch, UTC).
DT now();

// Minutes for "30m"/"2h"/"1d"; throws std::runtime_error on invalid.
long long parse_duration(const std::string& s);

// Parse a schedule string into a schedule Json dict {kind, ...}. `now` is used
// only for relative one-shot durations. Throws std::runtime_error on invalid.
core::Json parse_schedule(const std::string& schedule, const DT& now);

// croniter.get_next: smallest minute strictly after `base` matching expr; nullopt
// if none within ~4 years. Supports *, lists, ranges, steps; Vixie dom/dow OR rule.
std::optional<DT> cron_next(const std::string& expr, const DT& base);

// Grace seconds for a schedule (half-period clamped [120,7200]).
long long compute_grace_seconds(const core::Json& schedule, const DT& now);

// One-shot eligibility window (ONESHOT_GRACE_SECONDS = 120).
std::optional<std::string> recoverable_oneshot_run_at(const core::Json& schedule,
                                                      const DT& now,
                                                      const std::optional<std::string>& last_run_at);

// Next run ISO string, or nullopt if no more runs.
std::optional<std::string> compute_next_run(const core::Json& schedule,
                                            const DT& now,
                                            const std::optional<std::string>& last_run_at);

}  // namespace cron
}  // namespace dd
