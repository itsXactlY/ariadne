// src/gateway/cli.cpp — impl of dd/gateway/cli.hpp.

#include "dd/gateway/cli.hpp"

#include "dd/core/json.hpp"
#include "dd/gateway/crypto.hpp"
#include "dd/gateway/devices.hpp"
#include "dd/gateway/identity.hpp"
#include "dd/gateway/relay.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <unistd.h>

#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>

namespace dd {
namespace gateway {

namespace fs = std::filesystem;
using core::Json;

std::filesystem::path state_dir(const fs::path& home_state) { return home_state / "gateway"; }

GatewayConfig config_from(const core::Config& cfg) {
    GatewayConfig g;
    auto pick = [&](const char* key, std::string& into) {
        if (const std::string v = cfg.get_string(key); !v.empty()) into = v;
        while (!into.empty() && into.back() == '/') into.pop_back();
    };
    pick("gateway.pod_url", g.pod_url);
    pick("gateway.bridge_url", g.bridge_url);
    pick("gateway.llm_url", g.llm_url);
    return g;
}

std::string relay_url_from(const core::Config& cfg) { return cfg.get_string("gateway.relay_url"); }
std::string relay_public_from(const core::Config& cfg) { return cfg.get_string("gateway.relay_public"); }

std::vector<std::string> split_scopes(const std::string& csv) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : csv + ",") {
        if (c == ',' || c == ' ') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    return out;
}

std::string lan_address() {
    ifaddrs* ifs = nullptr;
    if (::getifaddrs(&ifs) != 0) return {};
    std::string best;
    int best_rank = 9;
    for (ifaddrs* i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        const uint32_t ip = ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr);
        int rank = 9;   // a home LAN first, then the other private ranges; never loopback or a bridge
        if ((ip >> 16) == 0xC0A8) rank = 0;                        // 192.168/16
        else if ((ip >> 24) == 10) rank = 1;                      // 10/8
        else if ((ip >> 20) == 0xAC1) rank = 2;                   // 172.16/12
        const std::string name = i->ifa_name ? i->ifa_name : "";
        if (name.rfind("docker", 0) == 0 || name.rfind("podman", 0) == 0 || name.rfind("veth", 0) == 0 ||
            name.rfind("br-", 0) == 0 || name.rfind("virbr", 0) == 0 || name.rfind("lxc", 0) == 0 || name.rfind("tun", 0) == 0)
            rank = 9;
        if (rank < best_rank) {
            char text[INET_ADDRSTRLEN];
            ::inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr, text, sizeof text);
            best = text;
            best_rank = rank;
        }
    }
    ::freeifaddrs(ifs);
    return best;
}

namespace {

void print_qr(const std::string& code) {
    if (::access("/usr/bin/qrencode", X_OK) != 0 && ::access("/usr/local/bin/qrencode", X_OK) != 0) {
        std::cout << "(install qrencode for a scannable QR here; the code below pastes the same)\n";
        return;
    }
    // The code is base64: no shell metacharacters, so it can stand in the command line as is.
    const std::string cmd = "qrencode -t ANSIUTF8 -m 2 '" + code + "'";
    std::fflush(stdout);
    if (std::system(cmd.c_str()) != 0) std::cout << "(qrencode failed; paste the code below instead)\n";
}

}  // namespace

int pair(const fs::path& dir, const core::Config& cfg, const PairOptions& opt) {
    std::string error;
    auto id = load_or_create(dir, error);
    if (!id) { std::cerr << "ariadne: gateway identity: " << error << "\n"; return 1; }
    Devices devices(dir / "devices.json");
    std::string token;
    std::optional<Device> d;
    if (opt.name.empty()) {
        // Mazemaker's pairing: the same code the gateway's /pair page shows, host-less (the app
        // finds the gateway by mDNS), carrying the one pairing token.
        auto t = devices.pairing_token(true, error);
        if (!t) { std::cerr << "ariadne: " << error << "\n"; return 1; }
        token = *t;
        d = devices.by_token(token);
    } else {
        // A device of its own: its own token, its own scopes, revocable on its own.
        d = devices.add(opt.name, opt.scopes, token, error);
        if (!d) { std::cerr << "ariadne: " << error << "\n"; return 1; }
    }
    const std::string host = opt.host;
    const std::string code = DeviceGateway::mazemaker_code(token, id->fp_b64(), relay_public_from(cfg), host);
    std::cout << (opt.name.empty() ? "Mazemaker pairing" : "paired") << " · " << d->id << " (" << d->name << ") · scopes ";
    for (size_t i = 0; i < d->scopes.size(); ++i) std::cout << (i ? "," : "") << d->scopes[i];
    std::cout << "\n";
    print_qr(code);
    std::cout << "\npairing code (Mazemaker app → Settings → Secure Gateway → scan or paste):\n" << code << "\n\n"
              << "host  " << (host.empty() ? "(none — the phone finds the gateway via mDNS, _mazemaker-gw._tcp)" : host) << "\n"
              << "fp    sha256/" << id->fp_b64() << "\n";
    if (const std::string relay = relay_public_from(cfg); !relay.empty())
        std::cout << "relay " << relay << "  (phones away from the LAN post to " << relay << "/gw/" << id->fp_hex() << ")\n";
    if (opt.name.empty())
        std::cout << "This is the code the gateway's pairing page (https://127.0.0.1:<port>/pair) shows. It carries the\n"
                     "pairing token: show it to your phones only. `ariadne --revoke " << d->id << "` ends it for every\n"
                     "phone paired with it; the next /pair mints a new one.\n";
    else
        std::cout << "The code carries this device's own token: show it to that phone only. `ariadne --revoke " << d->id
                  << "` ends it.\n";
    return 0;
}

int list_devices(const fs::path& dir) {
    Devices devices(dir / "devices.json");
    const auto all = devices.list();
    if (all.empty()) { std::cout << "no paired devices (" << devices.file().string() << ")\n"; return 0; }
    for (const auto& d : all) {
        char when[32] = "?";
        const std::time_t t = static_cast<std::time_t>(d.created);
        std::strftime(when, sizeof when, "%Y-%m-%d %H:%M", std::localtime(&t));
        std::cout << d.id << "  " << when << "  ";
        for (size_t i = 0; i < d.scopes.size(); ++i) std::cout << (i ? "," : "") << d.scopes[i];
        std::cout << "  " << d.name << "\n";
    }
    std::string error;
    if (auto id = load(dir / "cert.pem", dir / "key.pem", error)) std::cout << "fp sha256/" << id->fp_b64() << "\n";
    return 0;
}

int revoke(const fs::path& dir, const std::string& id) {
    Devices devices(dir / "devices.json");
    std::string error;
    if (!devices.revoke(id, error)) { std::cerr << "ariadne: " << error << "\n"; return 1; }
    std::cout << "revoked " << id << ": its token and every session it opened are refused from the next request on\n";
    return 0;
}

int adopt(const fs::path& dir, const fs::path& hc) {
    std::string error;
    // Idempotent: a home that already holds THIS identity (same fp) only takes the token again,
    // so running it twice -- or after an upgrade that learned something new to adopt -- is safe.
    // A different identity is still refused: phones pinned the one they paired with.
    auto theirs = load(hc / "gateway.crt", hc / "gateway.key", error);
    if (!theirs) { std::cerr << "ariadne: " << error << "\n"; return 1; }
    std::string mine_err;
    auto mine = load(dir / "cert.pem", dir / "key.pem", mine_err);
    if (mine && mine->fp_b64() == theirs->fp_b64()) {
        std::cout << "identity already adopted from " << hc.string() << " · fp sha256/" << mine->fp_b64() << "\n";
    } else {
        if (!gateway::adopt(hc / "gateway.crt", hc / "gateway.key", dir, error)) { std::cerr << "ariadne: " << error << "\n"; return 1; }
        std::cout << "identity adopted from " << hc.string() << " · fp sha256/" << theirs->fp_b64() << "\n";
    }
    std::ifstream tf(hc / "gateway.token");
    std::string token;
    std::getline(tf, token);
    while (!token.empty() && (token.back() == '\n' || token.back() == '\r' || token.back() == ' ')) token.pop_back();
    if (token.empty()) {
        std::cout << "no gateway.token there: pair the phones again with `ariadne --pair`\n";
        return 0;
    }
    Devices devices(dir / "devices.json");
    auto d = devices.by_token(token);
    if (!d) d = devices.add("hermes-crypto (adopted)", Devices::default_scopes(), token, error);
    if (!d) { std::cerr << "ariadne: " << error << "\n"; return 1; }
    if (!devices.set_pairing_token(token, error)) { std::cerr << "ariadne: " << error << "\n"; return 1; }
    std::cout << "token adopted as " << d->id << " and as the Mazemaker pairing token: phones paired with\n"
                 "hermes-crypto keep working, and the /pair page shows the code they were paired with\n";
    return 0;
}

}  // namespace gateway
}  // namespace dd
