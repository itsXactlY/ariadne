// The wire's send policy: which failures a request is re-sent for, how long the pause
// before the second send is, and what a re-send would cost once the stream has already
// spoken. A fake endpoint on loopback is the only way to count sends — curl's own answer
// cannot tell you how many times it connected, so the server counts connections instead.
#include "dd/agent/http_llm.hpp"
#include "dd/agent/retry.hpp"
#include "dd/net/http.hpp"

#include <curl/curl.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "dd/core/message.hpp"

using dd::agent::CallStats;
using dd::agent::HttpLLM;
using dd::agent::WireFailure;
using dd::agent::WireResult;
using dd::core::Message;
using dd::core::json_arr;

static int g_fail = 0;
static void chk(bool ok, const std::string& label) {
    if (!ok) { ++g_fail; std::printf("FAIL %s\n", label.c_str()); }
}

// ── A one-shot HTTP endpoint on loopback ───────────────────────────────────────
// It answers each connection with the next response in `answers`. Nothing is ever
// answered twice, so a third connection is a failure the test can see rather than a
// hang it has to explain.
struct FakeEndpoint {
    std::vector<std::string> answers;
    std::atomic<int> served{0};
    std::atomic<bool> stop{false};
    int fd = -1;
    uint16_t port = 0;
    std::thread th;
    // A keep-alive server holds the socket open after the answer and waits for the next
    // request on it. Every real backend does this (llama-server, vLLM, OpenAI), so a
    // reader that ends a turn on the socket rather than on the stream's terminator is
    // only caught by a server that keeps the connection — which `hold_ms` stages.
    int hold_ms = 0;
    std::vector<std::pair<int, std::chrono::steady_clock::time_point>> held_;

    FakeEndpoint(std::vector<std::string> a, int answers_after = 1) : answers(std::move(a)) {
        for (int i = 0; i < answers_after; ++i) answers.push_back(kClosed);
    }

    static const std::string kClosed;   // a server with nothing left to say

    void start() {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || ::listen(fd, 8) != 0) {
            std::printf("FAIL cannot bind a loopback endpoint\n");
            ++g_fail;
            return;
        }
        socklen_t len = sizeof(a);
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
        th = std::thread([this] { run(); });
    }

    void halt() {
        stop.store(true);
        if (th.joinable()) th.join();
        if (fd >= 0) { ::close(fd); fd = -1; }
        for (const auto& h : held_) ::close(h.first);
        held_.clear();
    }

    void run() {
        for (;;) {
            // A held socket that ran out its hold closes: the test cannot hang on one.
            for (size_t i = 0; i < held_.size();) {
                if (std::chrono::steady_clock::now() >= held_[i].second) { ::close(held_[i].first); held_.erase(held_.begin() + static_cast<long>(i)); }
                else ++i;
            }
            pollfd p{fd, POLLIN, 0};
            if (::poll(&p, 1, 150) <= 0) {
                if (stop.load()) return;
                continue;
            }
            const int c = ::accept(fd, nullptr, nullptr);
            if (c < 0) return;

            // Read the request whole: a body left unread makes curl see a broken pipe on
            // its own write, which is a hiccup the test did not stage.
            std::string req;
            size_t need = 0;
            bool headers = false;
            while (!headers || req.size() < need) {
                char buf[2048];
                const ssize_t n = ::recv(c, buf, sizeof(buf), 0);
                if (n <= 0) break;
                req.append(buf, static_cast<size_t>(n));
                if (!headers && req.find("\r\n\r\n") != std::string::npos) {
                    headers = true;
                    const size_t body_at = req.find("\r\n\r\n") + 4;   // first byte of the body
                    const size_t at = req.find("Content-Length: ");
                    // Content-Length counts the body, not the bytes seen so far. The body is
                    // usually already in this very read, so adding the length to req.size()
                    // waits for bytes that never come.
                    need = body_at + (at < body_at ? std::strtoul(req.c_str() + at + 16, nullptr, 10) : 0);
                }
            }
            // The cut-off point: a cancelled call connects but never asks. Serving nothing
            // for it is what keeps "zero retries" true rather than merely unobserved.
            if (!headers) { ::close(c); continue; }

            const int n = ++served;
            const std::string& resp = n <= static_cast<int>(answers.size()) ? answers[n - 1] : kClosed;
            if (resp != kClosed) ::send(c, resp.data(), resp.size(), MSG_NOSIGNAL);
            if (hold_ms > 0 && resp != kClosed) held_.emplace_back(c, std::chrono::steady_clock::now() + std::chrono::milliseconds(hold_ms));
            else ::close(c);
        }
    }
};

const std::string FakeEndpoint::kClosed = "";

// A response with the body's own length in the header, so curl reads exactly what is there.
// `extra` is how a server adds a header a body cannot carry — Retry-After, here.
static std::string http(long status, const std::string& body, const std::string& ctype = "application/json",
                        const std::string& extra = "") {
    const char* reason = status == 200 ? "OK"
                       : status == 429 ? "Too Many Requests"
                       : status == 500 ? "Internal Server Error"
                       : status == 501 ? "Not Implemented"
                       : status == 503 ? "Service Unavailable"
                       : "Bad Request";
    std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n";
    head += "Content-Type: " + ctype + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" +
            extra + "Connection: close\r\n\r\n";
    return head + body;
}

static const char* kSse =
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"\"},\"finish_reason\":null}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"2 + 2 = 4\"},\"finish_reason\":null}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":9}}\n\n"
    "data: [DONE]\n\n";

static HttpLLM endpoint_llm(const FakeEndpoint& ep) {
    HttpLLM llm;
    llm.base_url = "http://127.0.0.1:" + std::to_string(ep.port) + "/v1";
    llm.model = "test-model";
    llm.stall_timeout_s = 5;   // a call that stops moving is a test failure, not a hang
    return llm;
}

static Message user(const std::string& text) {
    Message m;
    m.role = "user";
    m.content = text;
    return m;
}

int main() {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // ── The policy itself: which failures a second send can turn into an answer ──
    chk(dd::agent::wire_failure(WireResult{CURLE_COULDNT_CONNECT, 0, false, false, false, false, false}) == WireFailure::Transient,
        "refused connect is transient");
    chk(dd::agent::wire_failure(WireResult{CURLE_RECV_ERROR, 0, false, false, false, false, false}) == WireFailure::Transient,
        "socket cut before a byte is transient");
    chk(dd::agent::wire_failure(WireResult{CURLE_OK, 500, false, false, false, false, false}) == WireFailure::Transient,
        "a 5xx that handed nothing out is transient");
    chk(dd::agent::wire_failure(WireResult{CURLE_OK, 500, false, false, true, false, false}) == WireFailure::Answer,
        "a 5xx after the stream spoke is an answer");
    chk(dd::agent::wire_failure(WireResult{CURLE_RECV_ERROR, 0, false, false, true, false, false}) == WireFailure::Answer,
        "a socket cut after the stream spoke is an answer");
    chk(dd::agent::wire_failure(WireResult{CURLE_OK, 400, false, false, false, false, false}) == WireFailure::Answer,
        "a 400 is an answer, not a hiccup");
    chk(dd::agent::wire_failure(WireResult{CURLE_OK, 200, false, false, false, false, true}) == WireFailure::Answer,
        "an error chunk inside the stream is an answer");
    chk(dd::agent::wire_failure(WireResult{CURLE_OK, 200, false, false, false, false, false}) == WireFailure::None,
        "a streamed answer is an answer");
    chk(dd::agent::wire_failure(WireResult{CURLE_ABORTED_BY_CALLBACK, 0, false, true, false, false, false}) == WireFailure::Cancelled,
        "a cut call is never transient");
    chk(dd::agent::wire_failure(WireResult{CURLE_OPERATION_TIMEDOUT, 0, true, false, false, false, false}) == WireFailure::Answer,
        "the stall clock answering is not a hiccup");
    chk(dd::agent::wire_failure(WireResult{CURLE_WRITE_ERROR, 0, false, false, false, true, false}) == WireFailure::Answer,
        "the byte cap answering is not a hiccup");
    // A keep-alive server holds the socket after `data: [DONE]`; curl aborts the transfer
    // there, so its write error is the reader stopping. A policy that read it as a socket
    // fact would re-send a stream whose words the caller already has.
    chk(dd::agent::wire_failure(WireResult{CURLE_WRITE_ERROR, 200, false, false, true, false, false, true}) == WireFailure::None,
        "a stream that reached its terminator is an answer");
    chk(!dd::agent::transient(dd::agent::wire_failure(WireResult{CURLE_WRITE_ERROR, 200, false, false, true, false, false, true})),
        "a completed stream is never re-sent");
    chk(dd::agent::transient(WireFailure::Transient) && !dd::agent::transient(WireFailure::Answer) &&
            !dd::agent::transient(WireFailure::Cancelled) && !dd::agent::transient(WireFailure::None),
        "only Transient is re-sent");

    // ── A rate limit is a "come back", not a verdict ──
    // 429 was the case this policy got wrong: the request was never refused, it was not
    // served yet. 501 and the rest of the 4xx are verdicts, and re-sending those is the toy.
    chk(dd::agent::wire_failure(WireResult{CURLE_OK, 429, false, false, false, false, false}) == WireFailure::Transient,
        "a 429 is transient");
    chk(dd::agent::wire_failure(WireResult{CURLE_OK, 408, false, false, false, false, false}) == WireFailure::Transient,
        "a 408 is transient");
    chk(dd::agent::wire_failure(WireResult{CURLE_OK, 503, false, false, false, false, false}) == WireFailure::Transient,
        "a 503 is transient");
    chk(dd::agent::wire_failure(WireResult{CURLE_OK, 501, false, false, false, false, false}) == WireFailure::Answer,
        "a 501 is an answer: a second send asks the same unimplementable thing");
    chk(dd::agent::wire_failure(WireResult{CURLE_OK, 401, false, false, false, false, false}) == WireFailure::Answer,
        "a 401 is an answer, not a hiccup");
    chk(dd::agent::wire_failure(WireResult{CURLE_OK, 429, false, false, true, false, false}) == WireFailure::Answer,
        "a 429 after the stream spoke is an answer");
    chk(!dd::agent::retryable_status(400) && !dd::agent::retryable_status(501) &&
            !dd::agent::retryable_status(505) && dd::agent::retryable_status(429) &&
            dd::agent::retryable_status(502) && dd::agent::retryable_status(504),
        "only the come-back states are retryable");

    // ── Retry-After is the server's instruction; the budget is the limit ──
    chk(dd::agent::wire_wait_ms(1, 2) >= 2000.0, "a Retry-After longer than the backoff wins");
    chk(dd::agent::wire_wait_ms(1, 0) >= 250.0 && dd::agent::wire_wait_ms(1, 0) <= 375.0,
        "no Retry-After leaves the backoff as the wait");
    chk(dd::agent::wire_wait_ms(3, 0, 250.0, 2000.0, 50000.0) > 0.0,
        "a wait inside the budget is allowed");
    chk(dd::agent::wire_wait_ms(1, 120, 250.0, 2000.0, 400.0) < 0.0,
        "a Retry-After past the budget refuses to send again instead of sleeping");
    chk(dd::agent::wire_wait_ms(20, 0, 250.0, 2000.0, 5000.0) > 0.0,
        "a capped backoff inside a larger budget is allowed");
    chk(dd::agent::wire_wait_ms(20, 0, 250.0, 2000.0, 500.0) < 0.0,
        "a 2s-capped backoff does not fit in 500ms: the send is refused, not taken early");

    // ── A rate limit climbs the operator's ladder, not the millisecond backoff ──
    // Three quick re-sends for a blip, then 1 s, 5 s, 15 s, 30 s, 1 min. The rungs are the
    // contract: a client that comes back inside the server's window spends a send to learn
    // nothing, and one that gives up after 250 ms loses a turn a minute would have saved.
    {
        struct Rung { int attempt; double lo, hi; };
        const Rung rungs[] = {{1, 250.0, 275.0}, {2, 500.0, 550.0}, {3, 1000.0, 1100.0},
                              {4, 1000.0, 1100.0}, {5, 5000.0, 5500.0}, {6, 15000.0, 16500.0},
                              {7, 30000.0, 33000.0}, {8, 60000.0, 66000.0}};
        for (const Rung& r : rungs) {
            const double w = dd::agent::wire_rate_wait_ms(r.attempt, 0);
            chk(w >= r.lo && w <= r.hi, "rate rung " + std::to_string(r.attempt) + " waits " +
                                            std::to_string(static_cast<long>(r.lo)) + " ms (" +
                                            std::to_string(static_cast<long>(w)) + ")");
        }
        chk(dd::agent::wire_rate_wait_ms(9, 0) >= 60000.0, "the minute rung repeats past its end");
        chk(dd::agent::wire_rate_wait_ms(1, 30) >= 30000.0, "a longer Retry-After beats the rung");
        chk(dd::agent::wire_rate_wait_ms(8, 0) > dd::agent::wire_rate_wait_ms(3, 0) * 10.0,
            "the ladder grows by orders of magnitude, not by the millisecond backoff's cap");
        chk(dd::agent::wire_rate_wait_ms(8, 0, 30000.0) < 0.0,
            "a rung past the budget refuses the send instead of taking it early");
        chk(dd::agent::wire_rate_wait_ms(1, 0, 5000.0) > 0.0, "a rung inside the budget is taken");
    }

    // ── The header parser both curl paths share ──
    chk(dd::net::retry_after_seconds("Retry-After: 7\r\n") == 7, "Retry-After: 7 reads as 7s");
    chk(dd::net::retry_after_seconds("retry-after: 12\r\n") == 12, "the header name is case-insensitive");
    chk(dd::net::retry_after_seconds("Retry-After: Wed, 21 Oct 2015 07:28:00 GMT\r\n") == 0,
        "the HTTP-date form is not guessed at");
    chk(dd::net::retry_after_seconds("Content-Type: text/event-stream\r\n") == 0,
        "another header names no wait");

    // ── The pause: milliseconds, doubling, capped, jittered on top ──
    const double a1 = dd::agent::wire_backoff_ms(1);
    const double a2 = dd::agent::wire_backoff_ms(2);
    const double a3 = dd::agent::wire_backoff_ms(3);
    const double a20 = dd::agent::wire_backoff_ms(20);
    chk(a1 >= 250.0 && a1 <= 375.0, "first backoff is 250ms plus jitter");
    chk(a2 >= 500.0 && a2 <= 750.0, "second backoff doubles");
    chk(a3 >= 1000.0 && a3 <= 1500.0, "third backoff doubles again");
    chk(a20 >= 2000.0 && a20 <= 3000.0, "the backoff is capped at 2s plus jitter");

    // ── A restarting server: 500 on the first connection, 200 on the second ──
    {
        FakeEndpoint ep({http(500, "{\"error\":{\"message\":\"model still loading\"}}"), http(200, kSse, "text/event-stream")}, 0);
        ep.start();
        HttpLLM llm = endpoint_llm(ep);
        int sends_seen = 0;
        double latency_seen = 0;
        llm.on_stats = [&](const CallStats& st) { sends_seen = st.attempts; latency_seen = st.latency_ms; };
        const auto t0 = std::chrono::steady_clock::now();
        std::string answer;
        bool threw = false;
        try { answer = llm.chat({user("hi")}, json_arr()).content; } catch (const std::exception&) { threw = true; }
        const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ep.halt();
        chk(!threw && answer == "2 + 2 = 4", "the second send answers the turn");
        chk(ep.served.load() == 2, "exactly two connections");
        chk(sends_seen == 2, "the reply says two sends");
        chk(elapsed >= 245.0, "the backoff was waited out, not skipped");
        chk(latency_seen >= 245.0, "latency counts the whole wait, not just the send that answered");
    }

    // ── A refused request: one send, no pause ──
    {
        FakeEndpoint ep({http(400, "{\"error\":{\"message\":\"Unexpected reasoning effort max\"}}")}, 0);
        ep.start();
        HttpLLM llm = endpoint_llm(ep);
        const auto t0 = std::chrono::steady_clock::now();
        std::string what;
        try { llm.chat({user("hi")}, json_arr()); } catch (const std::exception& e) { what = e.what(); }
        const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ep.halt();
        chk(what.find("Unexpected reasoning effort max") != std::string::npos, "the server's own words come back");
        chk(ep.served.load() == 1, "a 400 is never re-sent");
        chk(elapsed < 200.0, "no backoff was paid for a 400");
    }

    // ── A stream that already spoke, then died: no second send, because the caller has words ──
    {
        std::string partial = std::string("data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"half\"},\"finish_reason\":null}]}\n\n");
        std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: " +
                           std::to_string(partial.size() + 100) + "\r\nConnection: close\r\n\r\n" + partial;
        FakeEndpoint ep({resp}, 0);
        ep.start();
        HttpLLM llm = endpoint_llm(ep);
        std::string streamed;
        llm.on_content = [&](const std::string& d) { streamed += d; };
        bool threw = false;
        try { llm.chat({user("hi")}, json_arr()); } catch (const std::exception&) { threw = true; }
        ep.halt();
        chk(streamed == "half", "the delta the caller got is the delta the caller keeps");
        chk(ep.served.load() == 1, "a stream that spoke is never re-sent");
        chk(threw, "a cut stream is reported, not silently answered twice");
    }

    // ── A call the operator cut: zero retries ──
    {
        FakeEndpoint ep({http(200, kSse, "text/event-stream")}, 0);
        ep.start();
        HttpLLM llm = endpoint_llm(ep);
        std::atomic<bool> interrupt{true};   // already cut before the call began
        llm.cancel = &interrupt;
        const auto t0 = std::chrono::steady_clock::now();
        std::string what;
        try { llm.chat({user("hi")}, json_arr()); } catch (const std::exception& e) { what = e.what(); }
        const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ep.halt();
        chk(what == "interrupted", "a cut call says it was cut");
        // curl may have written the request before its progress callback sees the flag, so
        // the proof is that the server was reached once and no pause was paid — never twice.
        chk(ep.served.load() <= 1, "a cut call never reached the server twice");
        chk(elapsed < 200.0, "a cut call pays no backoff");
    }

    // ── A keep-alive server: the terminator ends the turn, not the socket ──
    // No Content-Length, no `Connection: close`, and the socket held open after the last
    // byte — exactly what llama-server and vLLM do. Without the terminator the transfer
    // sits on that socket until the stall clock fires, so every turn costs the stall
    // timeout and is filed as a stalled server.
    {
        const std::string body = std::string(kSse);
        const std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n\r\n" + body;
        FakeEndpoint ep({resp}, 0);
        ep.hold_ms = 4000;   // longer than the call's 5s stall guard: a hang would show as a stall
        ep.start();
        HttpLLM llm = endpoint_llm(ep);
        const auto t0 = std::chrono::steady_clock::now();
        std::string answer;
        bool threw = false;
        try { answer = llm.chat({user("hi")}, json_arr()).content; } catch (const std::exception&) { threw = true; }
        const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ep.halt();
        chk(!threw && answer == "2 + 2 = 4", "a keep-alive stream is answered whole");
        chk(elapsed < 1000.0, "the turn ends at the terminator, not at the stall clock");
        chk(ep.served.load() == 1, "a completed turn is never re-sent");
    }

    // ── DD_WIRE_ATTEMPTS=1 is the old one-shot wire ──
    {
        FakeEndpoint ep({http(500, "{\"error\":{\"message\":\"model still loading\"}}")}, 0);
        ep.start();
        HttpLLM llm = endpoint_llm(ep);
        llm.retry_attempts = 1;
        std::string what;
        try { llm.chat({user("hi")}, json_arr()); } catch (const std::exception& e) { what = e.what(); }
        ep.halt();
        chk(what.find("model still loading") != std::string::npos, "the failure still names the server's reason");
        chk(what.find("sends") == std::string::npos, "a single send is not counted as a retry");
        chk(ep.served.load() == 1, "one attempt means one connection");
    }

    // ── A prewarm fill takes the same policy ──
    {
        FakeEndpoint ep({http(500, "{}"), http(200, "{}")}, 0);
        ep.start();
        HttpLLM llm = endpoint_llm(ep);
        const bool filled = llm.prime(llm.body({user("hi")}, json_arr()), nullptr, 2000);
        ep.halt();
        chk(filled, "the cache fill lands on the second send");
        chk(ep.served.load() == 2, "the prewarm re-sent once, no more");
    }

    // ── A rate limit: 429 with Retry-After, then the answer ──
    {
        FakeEndpoint ep({http(429, "{\"error\":{\"message\":\"rate limit\"}}", "application/json", "Retry-After: 1\r\n"),
                         http(200, kSse, "text/event-stream")}, 0);
        ep.start();
        HttpLLM llm = endpoint_llm(ep);
        int sends_seen = 0;
        llm.on_stats = [&](const CallStats& st) { sends_seen = st.attempts; };
        const auto t0 = std::chrono::steady_clock::now();
        std::string answer;
        bool threw = false;
        try { answer = llm.chat({user("hi")}, json_arr()).content; } catch (const std::exception&) { threw = true; }
        const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ep.halt();
        chk(!threw && answer == "2 + 2 = 4", "a 429 is waited out and the turn answers");
        chk(ep.served.load() == 2, "a 429 costs exactly one more connection");
        chk(sends_seen == 2, "the reply says two sends");
        chk(elapsed >= 990.0, "the server's Retry-After was waited out, not the 250ms backoff");
    }

    // ── Three rate limits in a row are waited out, not three sends and a dead turn ──
    {
        const std::string limited = http(429, "{\"error\":{\"message\":\"rate limit\"}}");
        FakeEndpoint ep({limited, limited, limited, http(200, kSse, "text/event-stream")}, 0);
        ep.start();
        HttpLLM llm = endpoint_llm(ep);
        int sends_seen = 0;
        llm.on_stats = [&](const CallStats& st) { sends_seen = st.attempts; };
        const auto t0 = std::chrono::steady_clock::now();
        std::string answer;
        bool threw = false;
        try { answer = llm.chat({user("hi")}, json_arr()).content; } catch (const std::exception&) { threw = true; }
        const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ep.halt();
        chk(!threw && answer == "2 + 2 = 4", "three 429s in a row still answer the turn");
        chk(ep.served.load() == 4, "the ladder's first three rungs are three more sends");
        chk(sends_seen == 4, "the reply says four sends");
        chk(elapsed >= 1700.0, "the quick rungs were waited out: 250 ms + 500 ms + 1 s (" +
                                   std::to_string(static_cast<long>(elapsed)) + " ms)");
        std::printf("rate ladder live: %d sends over %ld ms (rungs 250/500/1000 ms)\n",
                    ep.served.load(), static_cast<long>(elapsed));
    }

    // ── A rate limit that asks for longer than the call may spend: give up and say why ──
    {
        FakeEndpoint ep({http(429, "{\"error\":{\"message\":\"rate limit\"}}", "application/json", "Retry-After: 120\r\n"),
                         http(200, kSse, "text/event-stream")}, 0);
        ep.start();
        HttpLLM llm = endpoint_llm(ep);
        llm.retry_budget_ms = 400;   // a turn that cannot wait two minutes
        const auto t0 = std::chrono::steady_clock::now();
        std::string what;
        try { llm.chat({user("hi")}, json_arr()); } catch (const std::exception& e) { what = e.what(); }
        const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ep.halt();
        chk(ep.served.load() == 1, "a wait past the budget is not taken: one connection");
        chk(elapsed < 1000.0, "the client does not sleep toward a failure it already knows");
        chk(what.find("rate limit") != std::string::npos, "the server's own words still come back");
        chk(what.find("120") != std::string::npos, "the failure names the wait the server asked for");
        chk(what.find("budget") != std::string::npos, "the failure says which limit stopped it");
    }

    // ── A verdict on the request: 501 is never re-sent ──
    {
        FakeEndpoint ep({http(501, "{\"error\":{\"message\":\"not implemented\"}}")}, 0);
        ep.start();
        HttpLLM llm = endpoint_llm(ep);
        const auto t0 = std::chrono::steady_clock::now();
        std::string what;
        try { llm.chat({user("hi")}, json_arr()); } catch (const std::exception& e) { what = e.what(); }
        const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ep.halt();
        chk(ep.served.load() == 1, "a 501 is never re-sent");
        chk(elapsed < 200.0, "no backoff was paid for a 501");
        chk(what.find("not implemented") != std::string::npos, "the 501's own words come back");
    }

    // ── What the operator asked the upstream for reaches the wire ──
    // One model is served by many providers, and only a request field says which one
    // answers; this transport has no key of its own for it, so the operator's fields
    // ride along verbatim. body() is a pure function, so the contract is checked on the
    // bytes it builds - no endpoint is needed, and none of it is policy we invent.
    {
        HttpLLM llm;
        llm.model = "the-model-we-chose";
        llm.extra = dd::core::json_parse(
            "{\"provider\":{\"only\":[\"deepseek\"],\"allow_fallbacks\":false},"
            "\"model\":\"a-name-the-operator-picked\"}");
        const std::string wire = dd::core::json_dump(llm.body({user("hi")}, json_arr()));
        chk(wire.find("\"only\":[\"deepseek\"]") != std::string::npos,
            "the provider pin reaches the wire verbatim");
        chk(wire.find("\"allow_fallbacks\":false") != std::string::npos,
            "refusing a substitute survives as false, not as a dropped key");
        chk(wire.find("\"model\":\"the-model-we-chose\"") != std::string::npos,
            "a name we decided is ours: the operator's field cannot replace it");
    }

    curl_global_cleanup();
    std::printf(g_fail == 0 ? "wire retry: all ok\n" : "wire retry: %d failed\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
