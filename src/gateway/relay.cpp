// src/gateway/relay.cpp — impl of dd/gateway/relay.hpp, over libcurl's WebSocket API.

#include "dd/gateway/relay.hpp"

#include "dd/core/json.hpp"
#include "dd/core/slot_ring.hpp"
#include "dd/gateway/crypto.hpp"

#include <curl/curl.h>
#include <poll.h>

#include <chrono>
#include <string>
#include <thread>
#include <algorithm>
#include <thread>

namespace dd {
namespace gateway {

using core::Json;

namespace {

using Clock = std::chrono::steady_clock;

struct Handle {
    CURL* c = nullptr;
    ~Handle() {
        if (c) curl_easy_cleanup(c);
    }
};

// What the worker threads hand back: whole frames ready to send. Shared, so a worker
// that outlives one relay session still has somewhere to put its reply -- until the
// session marks the outbox dead, which is the only way a reply can stop waiting.
using Outbox = relay_detail::Outbox;

curl_socket_t sock_of(CURL* c) {
    curl_socket_t s = CURL_SOCKET_BAD;
    curl_easy_getinfo(c, CURLINFO_ACTIVESOCKET, &s);
    return s;
}

bool wait_fd(CURL* c, short events, int ms) {
    pollfd p{static_cast<int>(sock_of(c)), events, 0};
    return ::poll(&p, 1, ms) > 0;
}

bool send_frame(CURL* c, const std::string& text, unsigned flags = CURLWS_TEXT) {
    size_t off = 0;
    for (int spins = 0; off < text.size() || (text.empty() && spins == 0); ++spins) {
        size_t sent = 0;
        const CURLcode rc = curl_ws_send(c, text.data() + off, text.size() - off, &sent, 0, flags);
        if (rc == CURLE_AGAIN) {
            if (!wait_fd(c, POLLOUT, 1000)) return false;
            continue;
        }
        if (rc != CURLE_OK) return false;
        // CURLE_OK with sent == 0 advances nothing: the loop would spin forever at 100%
        // CPU on the socket thread, never draining the outbox and stalling every worker.
        // No progress is a failed send, not a retryable one.
        if (sent == 0) return false;
        off += sent;
        if (text.empty()) break;
    }
    return true;
}

// One whole message, or "" with `got` false when none is complete within `ms`.
// `closed` is set when the relay closed the socket or it broke.
std::string recv_message(CURL* c, int ms, bool& got, bool& closed) {
    got = false;
    std::string msg;
    char buf[65536];
    const auto deadline = Clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
        size_t n = 0;
        const struct curl_ws_frame* meta = nullptr;
        const CURLcode rc = curl_ws_recv(c, buf, sizeof buf, &n, &meta);
        if (rc == CURLE_AGAIN) {
            const auto p = relay_detail::again_policy(deadline, Clock::now(), msg.size());
            if (p.give_up) return {};
            if (!wait_fd(c, POLLIN, p.wait_ms)) return {};
            continue;
        }
        if (rc != CURLE_OK) { closed = true; return {}; }
        if (meta && (meta->flags & CURLWS_CLOSE)) { closed = true; return {}; }
        if (meta && (meta->flags & (CURLWS_PING | CURLWS_PONG))) continue;   // curl answers pings itself
        msg.append(buf, n);
        if (meta && meta->bytesleft == 0 && !(meta->flags & CURLWS_CONT)) {
            got = true;
            return msg;
        }
    }
}

std::string str_of(const Json* v) { return v && v->type == Json::Str ? v->str : std::string(); }

}  // namespace

namespace relay_detail {

// A worker's reply waits for the socket thread only while its session can still send it.
bool publish_reply(Outbox& out, const std::string& frame, const std::atomic<int>& stop, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    // The liveness check comes FIRST. Checking it only after a failed publish lets a
    // session that already ended accept the reply: nothing is left to drain the ring, so
    // the frame is held forever instead of dropped, and the timeout never elapses.
    for (;;) {
        if (stop.load(std::memory_order_acquire) || out.dead.load(std::memory_order_acquire)) return false;
        if (out.ring.publish_any(frame)) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

AgainPolicy again_policy(std::chrono::steady_clock::time_point deadline, std::chrono::steady_clock::time_point now,
                         size_t have) {
    if (have >= kMaxRelayMessage) return {true, 0};
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    // A frame that stopped arriving does not extend the deadline.
    if (left.count() <= 0) return {true, 0};
    return {false, static_cast<int>(std::min<long>(left.count(), 250))};
}

}  // namespace relay_detail

std::string RelayClient::phone_base(const std::string& relay_ws_url, const Identity& id) {
    std::string base = relay_ws_url;
    if (base.rfind("wss://", 0) == 0) base = "https://" + base.substr(6);
    else if (base.rfind("ws://", 0) == 0) base = "http://" + base.substr(5);
    if (base.size() >= 3 && base.compare(base.size() - 3, 3, "/gw") == 0) base.resize(base.size() - 3);
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base + "/gw/" + id.fp_hex();
}

bool RelayClient::session(const std::atomic<int>& stop, std::string& why) {
    Handle h;
    h.c = curl_easy_init();
    if (!h.c) { why = "no curl handle"; return false; }
    curl_easy_setopt(h.c, CURLOPT_URL, url_.c_str());
    curl_easy_setopt(h.c, CURLOPT_CONNECT_ONLY, 2L);   // 2: upgrade to WebSocket, then hand us the socket
    curl_easy_setopt(h.c, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(h.c, CURLOPT_NOSIGNAL, 1L);
    if (const CURLcode rc = curl_easy_perform(h.c); rc != CURLE_OK) { why = curl_easy_strerror(rc); return false; }

    bool got = false, closed = false;
    std::string m = recv_message(h.c, 30000, got, closed);
    Json chal;
    try { if (got) chal = core::json_parse(m); } catch (const std::exception&) {}
    if (!got || str_of(core::json_get(chal, "type")) != "challenge") { why = "no challenge from the relay"; return false; }
    const std::string sig = sign_sha256(id_, str_of(core::json_get(chal, "nonce")));
    if (sig.empty()) { why = "cannot sign the relay's nonce"; return false; }
    Json reg = core::json_obj();
    core::obj_set(reg, "fp", core::json_str(id_.fp_hex()));
    core::obj_set(reg, "cert", core::json_str(crypto::b64(id_.cert_der())));
    core::obj_set(reg, "sig", core::json_str(crypto::b64(sig)));
    if (!send_frame(h.c, core::json_dump(reg))) { why = "registration send failed"; return false; }
    m = recv_message(h.c, 30000, got, closed);
    Json ok;
    try { if (got) ok = core::json_parse(m); } catch (const std::exception&) {}
    if (!got || str_of(core::json_get(ok, "type")) != "registered") {
        why = "registration refused: " + (got ? m.substr(0, 200) : std::string("no answer"));
        return false;
    }
    std::fprintf(stderr, "ariadne gateway: relay registered fp=%s… at %s\n", id_.fp_hex().substr(0, 16).c_str(), url_.c_str());

    auto out = std::make_shared<Outbox>();
    auto last_ping = Clock::now();
    while (stop.load(std::memory_order_relaxed) == 0) {
        std::string frame;
        while (out->ring.claim_one(frame))   // replies the workers finished
            if (!send_frame(h.c, frame)) { why = "send failed"; return true; }
        if (Clock::now() - last_ping > std::chrono::seconds(25)) {   // keeps NAT and the relay's heartbeat happy
            if (!send_frame(h.c, std::string(), CURLWS_PING)) { why = "ping failed"; return true; }
            last_ping = Clock::now();
        }
        m = recv_message(h.c, 100, got, closed);
        if (closed) { why = "the relay closed the connection"; return true; }
        if (!got) continue;
        Json f;
        try { f = core::json_parse(m); } catch (const std::exception&) { continue; }
        if (str_of(core::json_get(f, "type")) != "request") continue;
        const std::string corr = str_of(core::json_get(f, "corr_id"));
        auto answer = [corr](int status, const std::string& body) {
            Json r = core::json_obj();
            core::obj_set(r, "corr_id", core::json_str(corr));
            core::obj_set(r, "status", core::json_num(status));
            core::obj_set(r, "body", core::json_str(body));
            return core::json_dump(r);
        };
        if (str_of(core::json_get(f, "path")) != "/command") {
            send_frame(h.c, answer(404, R"({"error":"the relay forwards /command only"})"));
            continue;
        }
        if (out->inflight.load(std::memory_order_acquire) >= kMaxInflight) {
            send_frame(h.c, answer(503, R"({"error":"gateway busy: too many relayed requests in flight"})"));
            continue;
        }
        out->inflight.fetch_add(1, std::memory_order_acq_rel);
        // The bearer the relay passed along ("auth") is ignored on purpose: via_relay
        // makes every command authenticate by an e2e session, never by token.
        std::thread([out, answer, body = str_of(core::json_get(f, "body")), this] {
            const Reply r = gw_.command(body, std::string(), true);
            std::string frame = answer(r.status, r.body);
            // A reply the session can no longer send is dropped, not held: the ring has
            // no drainer once session() returns, and spinning on it is a leaked thread.
            // `stop` is not capturable here: this thread outlives session(), and `stop` is
            // the caller's atomic. The outbox's own dead flag is the lifetime-safe signal,
            // set when session() returns.
            relay_detail::publish_reply(*out, frame, out->dead, 5000);
            out->inflight.fetch_sub(1, std::memory_order_acq_rel);
        }).detach();
    }
    why = "stopping";
    // Anything still in flight finds a dead outbox and drops rather than waiting forever.
    out->dead.store(1, std::memory_order_release);
    // `dead` only stops a worker from *sending*: the thread that is still inside
    // gw_.command captured `this`, so returning here lets the caller destroy the
    // RelayClient (and its gateway) underneath a live worker. The outbox is shared_ptr'd,
    // the gateway is not — wait for the in-flight count to drain before unwinding.
    const auto drain = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (out->inflight.load(std::memory_order_acquire) > 0 &&
           std::chrono::steady_clock::now() < drain)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return true;
}

void RelayClient::run(const std::atomic<int>& stop) {
    int backoff = 2;
    while (stop.load(std::memory_order_relaxed) == 0) {
        std::string why;
        if (session(stop, why)) backoff = 2;
        if (stop.load(std::memory_order_relaxed)) break;
        std::fprintf(stderr, "ariadne gateway: relay %s: %s — retry in %ds\n", url_.c_str(), why.c_str(), backoff);
        for (int i = 0; i < backoff * 10 && stop.load(std::memory_order_relaxed) == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        backoff = backoff * 2 > 60 ? 60 : backoff * 2;
    }
}

}  // namespace gateway
}  // namespace dd
