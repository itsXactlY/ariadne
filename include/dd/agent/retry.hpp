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
// server or a fact about the socket. Only a socket fact can change on a second send, so
// only a socket fact is worth sending again. Everything the server answered — a 4xx, a
// body past the byte cap, an error chunk inside the stream, a call the operator cut — is
// the answer this request gets every time it is sent; re-sending it spends a turn to get
// the same words back, and on a stream it spends them twice.
enum class WireFailure {
    None,       // an acceptable answer arrived
    Answer,     // the server answered, but not with what was asked for: re-sending changes nothing
    Cancelled,  // curl stopped because the operator said stop; nobody asked for the call again
    Transient,  // no answer arrived and nothing reached the caller: refused connect, cut socket, 5xx
};

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
};

WireFailure wire_failure(const WireResult& r);

// True for Transient only — the one class a re-send can turn into an answer.
bool transient(WireFailure f);

// The pause before send `attempt` (1-based). The same decorrelation as jittered_backoff,
// scaled to the wire: a socket hiccup clears in milliseconds, not in the 5s an agent turn
// waits for a rate limit. A restarting server is back well before the second send.
double wire_backoff_ms(int attempt, double base_ms = 250.0, double max_ms = 2000.0);

}  // namespace agent
}  // namespace dd
