// Pure subset of agent/retry_utils.py (see header for the porting boundary).
#include "dd/agent/retry_utils.hpp"
#include <cmath>
#include <algorithm>

namespace dd {
namespace agent {

double backoff_delay(int attempt, double base_delay, double max_delay) {
    int exponent = attempt - 1;
    if (exponent < 0) exponent = 0;
    if (exponent >= 63 || base_delay <= 0.0) return max_delay;
    double raw = base_delay * std::pow(2.0, exponent);
    return std::min(raw, max_delay);
}

}  // namespace agent
}  // namespace dd
