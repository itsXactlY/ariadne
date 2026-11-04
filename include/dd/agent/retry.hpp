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

// ── The wire's own retry policy ───────────────────────────────────────────────
// One send of a request is an experiment, and its outcome is either a fact about the
// server or a state the server is in. A state that says "come back" can differ on a second
// send; a verdict on the request cannot. So a refused connect, a cut socket, a 5xx that
// handed nothing out, a rate limit, and a provider that failed *before the first token* are
// worth one more send — everything else the server answered (a 400, a 401, a 501, a body past
// the byte cap, an error chunk after a delta has already reached the caller, a call the
// operator cut) is the answer this request gets every time it is sent; re-sending it spends a
// turn to get the same words back, and on a stream it spends them twice. A 429 is the case
// worth naming: the request was never refused, it was not served *yet*, and the server says
// when to ask again (see `retry_after_s`). The other is an upstream provider's own failure,
// which OpenRouter delivers as an error chunk inside an otherwise 200 stream ("Upstream error
// from <provider>: The response stream was interrupted ... Please retry the request.") — the
// server asks for the retry in words, and a turn used to die on it.
enum class WireFailure {
    None,       // an acceptable answer arrived
    Answer,     // the server answered, but not with what was asked for: re-sending changes nothing
    Cancelled,  // curl stopped because the operator said stop; nobody asked for the call again
    Transient,  // nothing reached the caller and a second send may differ: cut socket, 5xx, 429, a provider's pre-token failure
};

// The server states that mean "ask again", never "this request is wrong": 408 (it gave up
// waiting for us), 429 (we are over its rate), 500/502/503/504 (it failed to serve this
// one). 501 and 505 are answers — a second send asks the same unimplementable thing — and
// every other 4xx is the request being refused. Retrying only this list is what makes the
// policy a policy rather than "any non-2xx twice".
bool retryable_status(long status);

// One send, in the terms the policy reads. curl's code travels as an int so this header
// stays curl-free; a non-zero code means no answer arrived at all. `spoken` is the whole
// rule for a streamed call: once reasoning or answer bytes have been handed to the caller,
// a second send would hand the same words out a second time.
struct WireResult {
    int code = 0;                // curl's CURLcode; 0 = CURLE_OK
    long status = 0;             // the response line, 0 when none arrived
    bool stalled = false;        // the stall clock fired: the server fell silent mid-call
    bool cancelled = false;      // curl aborted because the cancel flag was set
    bool spoken = false;         // reasoning or answer bytes already reached the caller
    bool refused = false;        // the response hit the byte cap, or the stream overflowed
    bool stream_error = false;   // an error chunk arrived inside an otherwise-ok stream
    // The stream reached its own terminator (`data: [DONE]`). curl then aborts the transfer
    // — a keep-alive server holds the connection open — so a non-zero curl code here is the
    // reader stopping, not the server failing. It must never be re-sent.
    bool completed = false;
    long retry_after_s = 0;      // the server's own Retry-After, 0 when it named none
};

WireFailure wire_failure(const WireResult& r);

// True for Transient only — the one class a re-send can turn into an answer.
bool transient(WireFailure f);

// The pause before send `attempt` (1-based). The same decorrelation as jittered_backoff,
// scaled to the wire: a socket hiccup clears in milliseconds, not in the 5s an agent turn
// waits for a rate limit. A restarting server is back well before the second send.
double wire_backoff_ms(int attempt, double base_ms = 250.0, double max_ms = 2000.0);

// What to actually wait before send `attempt`, which is not the same question.
// `retry_after_s` is the server's own instruction: a wait shorter than it earns another 429,
// so the longer of the two wins. `remaining_ms` is what the call's retry budget has left
// (-1 = unbounded); a wait that would run past it returns -1, so the caller stops and says
// what the server asked for instead of sleeping into a failure it already knows.
double wire_wait_ms(int attempt, long retry_after_s, double base_ms = 250.0,
                    double max_ms = 2000.0, double remaining_ms = -1.0);

// A rate limit is not a socket hiccup, and 250 ms of backoff is the wrong scale for it: the
// server named a window, and coming back inside it spends a send to learn nothing. The ladder
// is the operator's: three quick re-sends for a blip (250 ms, 500 ms, 1 s), then the server's
// own timescale -- 1 s, 5 s, 15 s, 30 s, 1 min -- with the last rung repeating, because a limit
// that still says no after a minute is still the server's call. The budget, not the ladder, is
// what ends the climb, and the server's own Retry-After wins over the rung when it is longer.
double wire_rate_wait_ms(int attempt, long retry_after_s, double remaining_ms = -1.0);

}  // namespace agent
}  // namespace dd
