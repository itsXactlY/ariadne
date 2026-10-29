#pragma once
// dd/gateway/devices.hpp — the devices a gateway answers: one bearer token each,
// stored hashed, with the scopes it was paired for.
//
// state/gateway/devices.json is written only by the CLI (`ariadne pair`, `revoke`,
// `--gateway-adopt`) and only by rename, so a reader sees one whole version. The
// gateway re-reads it on every authentication instead of caching it: a revoked
// phone is refused on its next request, and nothing is shared between connection
// threads.
//
// Scopes:
//   chat    the agent's chat surface (/v1/chat/completions), cmd:bridge and cmd:llm
//   memory  cmd:pod -- the Mazemaker pod behind the gateway
//   tools   the agent may run its file and shell tools for this device
//   admin   cmd:status details and the device list
//
// The Mazemaker pairing token. Pairing a phone is Mazemaker's flow: the Architect opens the
// gateway's loopback-only page (/pair), the app scans the QR. That page hands out one token,
// as hermes-crypto's gateway.token did -- so it is the one secret kept in clear, in
// pairing.token beside devices.json (0600), with its device row named "mazemaker pairing".
// Adopting hermes-crypto makes its gateway.token this token, so phones paired there match.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dd {
namespace gateway {

struct Device {
    std::string id, name;
    std::string token_sha256;   // hex
    std::vector<std::string> scopes;
    long long created = 0;      // unix seconds
    bool has(std::string_view scope) const;
};

class Devices {
public:
    explicit Devices(std::filesystem::path file) : file_(std::move(file)) {}

    std::vector<Device> list() const;
    std::optional<Device> by_token(std::string_view token) const;
    std::optional<Device> by_id(std::string_view id) const;
    // Add a device. An empty `token` mints one (32 random bytes, hex); `token` returns
    // it, the only time it exists in clear. nullopt + error when the file cannot be written.
    std::optional<Device> add(const std::string& name, const std::vector<std::string>& scopes, std::string& token,
                              std::string& error);
    bool revoke(std::string_view id, std::string& error);

    // The token the Mazemaker pairing page and `ariadne --pair` hand out. `create` mints it
    // (and its device row) when there is none; nullopt + error when that fails, or when there
    // is none and `create` is false.
    std::optional<std::string> pairing_token(bool create, std::string& error);
    // Make `token` the pairing token (adoption). Its device row must exist or is added.
    bool set_pairing_token(const std::string& token, std::string& error);
    std::filesystem::path pairing_file() const { return file_.parent_path() / "pairing.token"; }

    const std::filesystem::path& file() const { return file_; }

    static const std::vector<std::string>& default_scopes();   // chat, memory, tools
    static bool known_scope(std::string_view scope);

private:
    bool save(const std::vector<Device>& all, std::string& error) const;
    std::filesystem::path file_;
};

}  // namespace gateway
}  // namespace dd
