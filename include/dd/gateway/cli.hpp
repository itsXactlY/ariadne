#pragma once
// dd/gateway/cli.hpp — the operator's side of the device gateway: pairing, the device
// list, revocation, adopting a hermes-crypto gateway, and the config it runs on.
//
//   ariadne --pair [--name N] [--scopes chat,memory,tools,admin] [--host HOST:PORT]
//       mints a device token and prints the pairing code (and a QR when qrencode is
//       installed). The code is what the Mazemaker app scans: base64(JSON{v, host,
//       token, fp[, relay]}), fp = base64(sha256(SPKI)) of this gateway's certificate.
//   ariadne --devices
//   ariadne --revoke ID
//   ariadne --gateway-adopt DIR
//       take over a hermes-crypto gateway: DIR's gateway.crt/gateway.key become this
//       gateway's identity (phones keep their pin) and gateway.token becomes a device
//       (phones keep their token). Nothing to re-pair.
//
// Config (config.yaml, all optional):
//   gateway:
//     pod_url:      http://127.0.0.1:8765      the Mazemaker pod behind cmd:pod
//     bridge_url:   http://127.0.0.1:8769      the agent bridge behind cmd:bridge
//     llm_url:      http://127.0.0.1:8080      a model server root on THIS machine (cmd:llm)
//     relay_url:    wss://relay.example/gw     dial out to a relay (G5); empty = LAN only
//     relay_public: https://relay.example      what a phone reaches the relay at (in the code)

#include <filesystem>
#include <string>
#include <vector>

#include "dd/core/config.hpp"
#include "dd/gateway/device_gateway.hpp"

namespace dd {
namespace gateway {

struct PairOptions {
    std::string name;
    std::vector<std::string> scopes;
    std::string host;   // HOST:PORT a phone dials; "" = this machine's LAN address and `port`
    int port = 8443;
};

std::filesystem::path state_dir(const std::filesystem::path& home_state);   // <home>/state/gateway
GatewayConfig config_from(const core::Config& cfg);
std::string relay_url_from(const core::Config& cfg);
std::string relay_public_from(const core::Config& cfg);

// Each returns a process exit status and prints what the operator needs.
int pair(const std::filesystem::path& dir, const core::Config& cfg, const PairOptions& opt);
int list_devices(const std::filesystem::path& dir);
int revoke(const std::filesystem::path& dir, const std::string& id);
int adopt(const std::filesystem::path& dir, const std::filesystem::path& hermes_crypto_home);

// The first private IPv4 of this machine, "" when there is none.
std::string lan_address();
std::vector<std::string> split_scopes(const std::string& csv);

}  // namespace gateway
}  // namespace dd
