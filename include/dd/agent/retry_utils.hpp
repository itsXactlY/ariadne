#pragma once
// Pure subset of agent/retry_utils.py.
// Ported: the deterministic backoff delay before jitter —
//   exponent = max(0, attempt-1)
//   delay = max_delay if (exponent>=63 or base_delay<=0) else min(base_delay*2^exp, max_delay)
// NOT ported (thread/time/RNG coupled): the jitter term (threading.Lock counter,
//   time.time_ns, Python Mersenne-Twister random.Random(seed).uniform).
#include <string>
#include <cmath>

namespace dd {
namespace agent {

double backoff_delay(int attempt, double base_delay, double max_delay);

}  // namespace agent
}  // namespace dd
