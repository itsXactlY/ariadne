// src/gateway/relay.cpp — impl of dd/gateway/relay.hpp, over libcurl's WebSocket API.

#include "dd/gateway/relay.hpp"

#include "dd/core/json.hpp"
#include "dd/core/slot_ring.hpp"
#include "dd/gateway/crypto.hpp"

#include <curl/curl.h>
#include <poll.h>

#include <chrono>
#include <cstdio>
#include <memory>
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
// that outlives one relay session still has somewhere to put its reply.
struct Outbox {
    core::SlotRing<64, std::string> ring{64};
    std::atomic<int> inflight{0};
};

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
            const int left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count());
            if (left <= 0 && msg.empty()) return {};
            if (!wait_fd(c, POLLIN, left > 0 ? left : 50) && msg.empty()) return {};
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
            while (!out->ring.publish_any(std::string(frame)))   // full: the socket thread drains it
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            out->inflight.fetch_sub(1, std::memory_order_acq_rel);
        }).detach();
    }
    why = "stopping";
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
