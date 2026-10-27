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

WireFailure wire_failure(const WireResult& r) {
    // A call the operator cut is not a failed call: re-sending it would ask for something
    // nobody asked for, and would do so after they had already moved on.
    if (r.cancelled) return WireFailure::Cancelled;

    // Both of these are a guard answering, not a hiccup: the byte cap and the stall clock
    // are the limits the caller set, and a second send reaches the same limit.
    if (r.refused || r.stalled) return WireFailure::Answer;

    if (r.code == 0) {   // curl reached the server and got a response line
        // A 5xx is the server failing to answer, not answering: if nothing was handed out
        // yet, the second send can get a different reply. A 4xx is an answer about the
        // request, so it comes back identically; so does an error chunk inside a stream.
        if (r.status >= 500 && !r.spoken) return WireFailure::Transient;
        return r.status >= 400 || r.stream_error ? WireFailure::Answer : WireFailure::None;
    }

    // No response line at all: refused connect, a socket closed mid-call, a timeout before
    // a byte. If the caller already has words, a second send would repeat them — the answer
    // they were given is the answer they keep. If it has none, the socket is the fact.
    if (r.spoken) return WireFailure::Answer;
    return WireFailure::Transient;
}

bool transient(WireFailure f) { return f == WireFailure::Transient; }

double wire_backoff_ms(int attempt, double base_ms, double max_ms) {
    // jittered_backoff works in seconds; the wire speaks milliseconds.
    return jittered_backoff(attempt, base_ms / 1000.0, max_ms / 1000.0, 0.5) * 1000.0;
}

}  // namespace agent
}  // namespace dd
