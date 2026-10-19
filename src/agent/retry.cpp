#include "dd/agent/retry.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <random>

namespace dd {
namespace agent {

// Python: _jitter_counter = 0; _jitter_lock = threading.Lock().
static std::uint64_t jitter_counter = 0;
static std::mutex jitter_lock;

double jittered_backoff(int attempt, double base_delay, double max_delay, double jitter_ratio) {
    std::uint64_t tick;
    {
        std::lock_guard<std::mutex> g(jitter_lock);
        tick = ++jitter_counter;
    }

    int exponent = attempt - 1;
    if (exponent < 0) exponent = 0;

    double delay;
    if (exponent >= 63 || base_delay <= 0.0) {
        delay = max_delay;
    } else {
        delay = std::min(base_delay * std::pow(2.0, exponent), max_delay);
    }

    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now().time_since_epoch()).count();
    std::uint64_t seed = (static_cast<std::uint64_t>(ns) ^ (tick * 0x9E3779B9ULL)) & 0xFFFFFFFFULL;
    std::mt19937_64 eng(seed);
    std::uniform_real_distribution<double> dist(0.0, jitter_ratio * delay);
    double jitter = dist(eng);

    return delay + jitter;
}

}  // namespace agent
}  // namespace dd
