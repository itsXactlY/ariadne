#pragma once
// dd/gateway/device_gateway.hpp — the device gateway: how a paired phone reaches this
// machine's agent, memory and model, on the LAN (G4) and through a relay (G5).
//
// It speaks the wire the Mazemaker app already speaks to hermes-crypto's lan_gateway,
// so a phone paired there needs nothing new on the LAN:
//
//   POST /command  {"cmd":"session"} + Authorization: Bearer <device token>
//                    -> {"created":{"session_id","client_key"}}       client_key: base64 AES-256
//                  {"session_id","enc":seal(client_key, {"cmd","args"})}
//                    -> {"enc":seal(client_key, result)}
//   POST /bridge/stream  (bearer) the agent bridge's /chat/stream, passed through as SSE
//   GET  /health   open
//   GET  / and /pair   the Mazemaker pairing page (QR + code), loopback only -- what the
//                      Architect's "pair your phone" opens on this host
//   GET  /pair.json    {code, fp}, loopback only; code = base64(JSON{v:1, token, fp}),
//                      host-less: the app finds the gateway by mDNS (_mazemaker-gw._tcp);
//                      v:2 + relay when a relay is published
//
// Commands: pod (the Mazemaker pod, scope memory), bridge (the agent bridge, scope chat),
// llm (this machine's local model, loopback only, scope chat), status, sessions, kill.
//
// Sessions hold no table. A session id is the session itself -- {device, key, expiry}
// sealed under a key this process draws at start -- so connection threads share
// nothing, a gateway restart ends every session (the app renegotiates on the
// "Decryption failed" answer), and a revoked device's sessions die with its row,
// because every request looks the device up again. Two marks are the exception, both
// held in this process and both bounded: a killed session and an answered e2e nonce.
// Without the first `kill` would answer "destroyed" and keep serving the session;
// without the second a captured handshake could be replayed forever inside the skew.
//
// End to end through a relay. On the LAN the token rides TLS to a pinned
// certificate. Through the relay it would be readable by the relay, and so would the
// v1 handshake's client_key. So a relayed request never carries a bearer; the
// session is opened with the e2e handshake instead, and the relay only ever sees
// sealed blobs:
//
//   {"cmd":"session","e2e":{"ts":<unix s>,"nonce":"<b64, 16 bytes>","proof":"<b64>"}}
//     proof = HMAC-SHA256(sha256(token), "ariadne-e2e-session|<ts>|<nonce>")
//   -> {"created":{"session_id","key_box","e2e":1}}
//     key_box = seal(HKDF-SHA256(ikm = sha256(token), salt = nonce bytes,
//                                info = "ariadne-e2e-session-key", 32), client_key)
//
// A replayed proof only yields another key_box that the replayer cannot open; |ts|
// must be within five minutes of this clock.

#include <optional>
#include <string>

#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "dd/core/json.hpp"
#include "dd/gateway/devices.hpp"
#include "dd/net/server.hpp"

namespace dd {
namespace gateway {

struct GatewayConfig {
    std::string pod_url = "http://127.0.0.1:8765";
    std::string bridge_url = "http://127.0.0.1:8769";
    std::string llm_url = "http://127.0.0.1:8080";   // a server root: cmd:llm paths are under v1/
    long proxy_timeout_ms = 900000;   // the gateway's deadline, never the caller's
    long session_ttl_s = 86400;
};

struct Reply {
    int status = 200;
    std::string body;
};

class DeviceGateway {
public:
    DeviceGateway(GatewayConfig cfg, Devices devices);

    // One POST /command. `bearer` is the presented token ("" when none); `via_relay`
    // marks a request the relay forwarded, which may never authenticate by bearer.
    Reply command(const std::string& body, const std::string& bearer, bool via_relay) const;

    // The gateway's own routes. false when the path is not one of them (the caller then
    // serves its agent surface, authenticating with device()).
    bool handle(const net::Request& req, net::Conn& conn) const;

    // The device behind a request's bearer token.
    std::optional<Device> device(const net::Request& req) const;

    // e2e helpers, shared with tests and any client written in C++.
    static std::string e2e_proof(const std::string& token, long long ts, const std::string& nonce_b64);
    static std::string e2e_wrap_key(const std::string& token, const std::string& nonce_raw);

    const GatewayConfig& config() const { return cfg_; }

    // What the pairing page needs: this gateway's fp (base64) and the relay a phone away from
    // the LAN reaches it at ("" when none). Without it /pair answers 503.
    void set_pairing(std::string fp_b64, std::string relay_public) {
        pair_fp_ = std::move(fp_b64);
        pair_relay_ = std::move(relay_public);
    }
    // The Mazemaker pairing code, exactly as hermes-crypto's pairing_code() made it.
    static std::string mazemaker_code(const std::string& token, const std::string& fp_b64,
                                      const std::string& relay_public, const std::string& host = "");

private:
    struct Session {
        Device device;
        std::string key;   // 32 raw bytes
    };
    // Marks are hashes, never the id or the nonce: the set must not hold a thing an
    // attacker can use. A nonce falls out when the cap is reached; a killed session's mark
    // lives until that session would have expired anyway. Evicting kill marks by an
    // insertion cap is a hole: enough later kills push the mark out and the killed session
    // keeps decrypting until its expiry, so `kill` becomes a lie.
    struct Marks {
        std::mutex mu;
        std::unordered_set<std::string> used_nonces;
        std::vector<std::string> nonce_order;
        std::unordered_map<std::string, long long> killed;   // hash -> unix time it stops mattering
    };
    mutable Marks marks_;
    void mark_killed(const std::string& session_id) const;
    bool is_killed(const std::string& session_id) const;
    bool nonce_replayed(const std::string& nonce_b64) const;   // records it; true when already answered

    std::string open_session(const Device& d, std::string& key_out) const;   // the session id
    std::optional<Session> resume(const std::string& session_id) const;
    core::Json dispatch(const std::string& cmd, const core::Json& args, const Device& d, const std::string& session_id) const;
    core::Json proxy(const std::string& base, const char* label, const core::Json& args, const char* path_prefix,
                     const std::string& down_message, bool force_no_stream) const;
    void bridge_stream(const net::Request& req, net::Conn& conn) const;
    void pairing(const net::Request& req, net::Conn& conn, bool json) const;

    GatewayConfig cfg_;
    Devices devices_;
    std::string secret_;   // seals session ids; drawn once, never leaves the process
    std::string pair_fp_, pair_relay_;
};

std::string bearer_of(const net::Request& req);   // the token after "Bearer ", "" otherwise

}  // namespace gateway
}  // namespace dd
