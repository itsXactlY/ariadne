#pragma once
// dd/gateway/relay.hpp — the gateway's side of the relay (G5): reach a phone outside
// the LAN with no inbound port on this machine.
//
// The gateway dials OUT to the relay over one WebSocket (wss://<relay>/gw), proves it
// owns its fingerprint -- it presents its certificate and signs the relay's nonce with
// the matching key, the protocol hermes-crypto's relay.py speaks -- and then answers
// every forwarded {corr_id, path:/command, body} through DeviceGateway::command with
// via_relay set, so a relayed request can never authenticate by bearer token: its
// session opens with the e2e handshake and the relay carries only sealed blobs.
//
// One thread owns the socket (a curl handle is not shareable). A forwarded request runs
// on a worker thread of its own, because a cmd:llm call can take minutes and must not
// hold the others; the worker hands its reply back through a SlotRing and the socket
// thread sends it. At most kMaxInflight requests run at once; past that the relay is
// answered 503 at once. The connection is retried with backoff (2 s, doubling, 60 s cap).

#include <atomic>
#include <string>

#include <chrono>

#include "dd/gateway/device_gateway.hpp"
#include "dd/gateway/identity.hpp"
#include "dd/core/slot_ring.hpp"

namespace dd {
namespace gateway {

// The relay's two wait policies, pulled out of relay.cpp. Neither is reachable from a
// unit test through a live socket -- both are only wrong when the peer stops sending --
// so the decision itself is the thing under test.
namespace relay_detail {

// The most a forwarded request may be. A relayed /command body is one sealed JSON; a
// peer that streams past this is broken, or holding a worker open on purpose.
constexpr size_t kMaxRelayMessage = 1u << 20;

struct AgainPolicy {
    bool give_up;   // end this request now
    int wait_ms;    // otherwise: how long to wait for more bytes
};

// What to do when curl has read every byte the socket holds and would block.
// The deadline is absolute: bytes that stop arriving end the request, even mid-frame.
// Holding onto a partial frame past the deadline is what parked the socket thread
// forever -- an idle relay with one half-delivered request never answered the next.
// Bytes that keep arriving may keep the frame going, up to kMaxRelayMessage.
AgainPolicy again_policy(std::chrono::steady_clock::time_point deadline,
                         std::chrono::steady_clock::time_point now, size_t have);

struct Outbox {
    core::SlotRing<64, std::string> ring{64};
    std::atomic<int> inflight{0};
    std::atomic<int> dead{0};   // set when the session ends: nothing can be sent any more
};

// Hand a worker's reply to the socket thread, waiting only while the session is alive.
// false when the reply is dropped: the caller still counts the request down, so a
// session that ended mid-request cannot leave its worker spinning on a ring no one drains.
bool publish_reply(Outbox& out, const std::string& frame, const std::atomic<int>& stop, int timeout_ms);

}  // namespace relay_detail

class RelayClient {
public:
    RelayClient(std::string url, Identity id, const DeviceGateway& gw)
        : url_(std::move(url)), id_(std::move(id)), gw_(gw) {}

    // Blocks: connect, register, serve; reconnect until `stop` is non-zero.
    void run(const std::atomic<int>& stop);

    // The relay URL a phone posts to for this gateway: <relay base>/gw/<fp hex>/command.
    static std::string phone_base(const std::string& relay_ws_url, const Identity& id);

    static constexpr int kMaxInflight = 32;

private:
    // One session with the relay; returns when it drops or `stop` is set. true when it
    // got as far as registered (the backoff resets).
    bool session(const std::atomic<int>& stop, std::string& why);

    std::string url_;
    Identity id_;
    const DeviceGateway& gw_;
};

}  // namespace gateway
}  // namespace dd
