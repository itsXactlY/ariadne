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

#include "dd/gateway/device_gateway.hpp"
#include "dd/gateway/identity.hpp"

namespace dd {
namespace gateway {

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
