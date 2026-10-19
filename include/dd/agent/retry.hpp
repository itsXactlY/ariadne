#pragma once
#include <cmath>
#include <cstdint>

namespace dd {
namespace agent {

// Faithful port of agent/retry_utils.py::jittered_backoff.
// Returns min(base_delay * 2^(attempt-1), max_delay) + uniform[0, jitter_ratio*delay].
// The jitter is seeded from a monotonic clock + a per-process counter (Python: time_ns ^
// (tick * 0x9E3779B9)); the exact value is non-deterministic by design (decorrelated retries),
// so the C++ MT19937 seeding need not bit-match Python's random.Random(seed).
double jittered_backoff(int attempt, double base_delay = 5.0, double max_delay = 120.0,
                        double jitter_ratio = 0.5);

}  // namespace agent
}  // namespace dd
