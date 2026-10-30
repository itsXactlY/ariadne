// The wire's send policy: which failures a request is re-sent for, how long the pause
// before the second send is, and what a re-send would cost once the stream has already
// spoken. A fake endpoint on loopback is the only way to count sends — curl's own answer
// cannot tell you how many times it connected, so the server counts connections instead.
#include "dd/agent/http_llm.hpp"
#include "dd/agent/retry.hpp"

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
static std::string http(long status, const std::string& body, const std::string& ctype = "application/json") {
    std::string head = status == 200 ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 " + std::to_string(status) + " " +
                       (status == 500 ? "Internal Server Error" : "Bad Request") + "\r\n";
    head += "Content-Type: " + ctype + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
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

    curl_global_cleanup();
    std::printf(g_fail == 0 ? "wire retry: all ok\n" : "wire retry: %d failed\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
