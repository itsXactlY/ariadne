#include "dd/agent/retry.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <atomic>
#include <cstdint>
#include <random>

namespace dd {
namespace agent {

// Python: _jitter_counter = 0; _jitter_lock = threading.Lock(). The lock bought one thing: a
// tick that differs per retry so two retries in the same nanosecond do not draw the same jitter.
// fetch_add gives that and nothing else — the tick is the only value the counter ever held, so
// there is no invariant a lock could protect. Any thread may retry at the same time.
static std::atomic<std::uint64_t> jitter_counter{0};

double jittered_backoff(int attempt, double base_delay, double max_delay, double jitter_ratio) {
    const std::uint64_t tick = jitter_counter.fetch_add(1, std::memory_order_relaxed);

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

    // The reader stopped at the stream's own terminator: the answer arrived, and curl's
    // abort code describes the reader, not the server. A re-send would duplicate the words.
    if (r.completed) return WireFailure::None;

    // Both of these are a guard answering, not a hiccup: the byte cap and the stall clock
    // are the limits the caller set, and a second send reaches the same limit.
    if (r.refused || r.stalled) return WireFailure::Answer;

    if (r.code == 0) {   // curl reached the server and got a response line
        // A state that says "come back" is not a verdict: if nothing was handed out yet, the
        // second send can get a different reply. A 400/401/501 is an answer about the
        // request, so it comes back identically; so does an error chunk inside a stream.
        if (retryable_status(r.status) && !r.spoken) return WireFailure::Transient;
        return r.status >= 400 || r.stream_error ? WireFailure::Answer : WireFailure::None;
    }

    // No response line at all: refused connect, a socket closed mid-call, a timeout before
    // a byte. If the caller already has words, a second send would repeat them — the answer
    // they were given is the answer they keep. If it has none, the socket is the fact.
    if (r.spoken) return WireFailure::Answer;
    return WireFailure::Transient;
}

bool transient(WireFailure f) { return f == WireFailure::Transient; }

bool retryable_status(long status) {
    return status == 408 || status == 429 || status == 500 || status == 502 ||
           status == 503 || status == 504;
}

double wire_backoff_ms(int attempt, double base_ms, double max_ms) {
    // jittered_backoff works in seconds; the wire speaks milliseconds.
    return jittered_backoff(attempt, base_ms / 1000.0, max_ms / 1000.0, 0.5) * 1000.0;
}

// A fixed rung plus a little decorrelation: without it every client that met the same limit
// comes back in the same millisecond, which is how one rate limit becomes a stampede.
static double rung_ms(double ms) {
    if (ms <= 0.0) return 0.0;
    const std::uint64_t tick = jitter_counter.fetch_add(1, std::memory_order_relaxed);
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now().time_since_epoch()).count();
    std::uint64_t seed = (static_cast<std::uint64_t>(ns) ^ (tick * 0x9E3779B9ULL)) & 0xFFFFFFFFULL;
    std::mt19937_64 eng(seed);
    std::uniform_real_distribution<double> dist(0.0, 0.1 * ms);
    return ms + dist(eng);
}

double wire_rate_wait_ms(int attempt, long retry_after_s, double remaining_ms) {
    // Three quick re-sends first: a limit that clears in a few hundred milliseconds should not
    // cost a turn a minute. After that the rungs are the ones the operator named, in seconds.
    static const double ladder[] = {250.0, 500.0, 1000.0, 1000.0, 5000.0, 15000.0, 30000.0, 60000.0};
    constexpr int rungs = static_cast<int>(sizeof(ladder) / sizeof(ladder[0]));
    int i = attempt - 1;
    if (i < 0) i = 0;
    if (i >= rungs) i = rungs - 1;   // the minute rung repeats; the budget ends the climb
    double wait = rung_ms(ladder[i]);
    if (retry_after_s > 0) {
        const double asked = static_cast<double>(retry_after_s) * 1000.0;
        if (asked > wait) wait = asked;   // the server's own window beats the ladder's rung
    }
    if (remaining_ms >= 0.0 && wait > remaining_ms) return -1.0;
    return wait;
}

double wire_wait_ms(int attempt, long retry_after_s, double base_ms, double max_ms,
                    double remaining_ms) {
    double wait = wire_backoff_ms(attempt, base_ms, max_ms);
    if (retry_after_s > 0) {
        const double asked = static_cast<double>(retry_after_s) * 1000.0;
        if (asked > wait) wait = asked;   // a shorter wait buys another 429, not an answer
    }
    if (remaining_ms >= 0.0 && wait > remaining_ms) return -1.0;
    return wait;
}

}  // namespace agent
}  // namespace dd
