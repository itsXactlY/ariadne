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

std::string pairing_code(const Identity& id, const std::string& host, const std::string& token, const std::string& relay_public) {
    Json doc = core::json_obj();
    core::obj_set(doc, "v", core::json_num(relay_public.empty() ? 1 : 2));
    if (!host.empty()) core::obj_set(doc, "host", core::json_str(host));
    core::obj_set(doc, "token", core::json_str(token));
    core::obj_set(doc, "fp", core::json_str(id.fp_b64()));
    if (!relay_public.empty()) core::obj_set(doc, "relay", core::json_str(relay_public));
    return crypto::b64(core::json_dump(doc));
}

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
    std::string name = opt.name;
    if (name.empty()) {
        char buf[32];
        const std::time_t t = std::time(nullptr);
        std::strftime(buf, sizeof buf, "phone %Y-%m-%d %H:%M", std::localtime(&t));
        name = buf;
    }
    auto d = devices.add(name, opt.scopes, token, error);
    if (!d) { std::cerr << "ariadne: " << error << "\n"; return 1; }
    std::string host = opt.host;
    if (host.empty()) {
        const std::string ip = lan_address();
        if (!ip.empty()) host = ip + ":" + std::to_string(opt.port);
    }
    const std::string code = pairing_code(*id, host, token, relay_public_from(cfg));
    std::cout << "paired " << d->id << " (" << d->name << ") · scopes ";
    for (size_t i = 0; i < d->scopes.size(); ++i) std::cout << (i ? "," : "") << d->scopes[i];
    std::cout << "\n";
    print_qr(code);
    std::cout << "\npairing code (Mazemaker app → Settings → Secure Gateway → scan or paste):\n" << code << "\n\n"
              << "host  " << (host.empty() ? "(none — the phone finds the gateway via mDNS)" : host) << "\n"
              << "fp    sha256/" << id->fp_b64() << "\n";
    if (const std::string relay = relay_public_from(cfg); !relay.empty())
        std::cout << "relay " << relay << "  (phones away from the LAN post to " << relay << "/gw/" << id->fp_hex() << ")\n";
    std::cout << "The code carries the device's token: show it to that phone only. `ariadne --revoke " << d->id
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
    if (!gateway::adopt(hc / "gateway.crt", hc / "gateway.key", dir, error)) { std::cerr << "ariadne: " << error << "\n"; return 1; }
    auto id = load(dir / "cert.pem", dir / "key.pem", error);
    std::cout << "identity adopted from " << hc.string() << " · fp sha256/" << (id ? id->fp_b64() : "?") << "\n";
    std::ifstream tf(hc / "gateway.token");
    std::string token;
    std::getline(tf, token);
    while (!token.empty() && (token.back() == '\n' || token.back() == '\r' || token.back() == ' ')) token.pop_back();
    if (token.empty()) {
        std::cout << "no gateway.token there: pair the phones again with `ariadne --pair`\n";
        return 0;
    }
    Devices devices(dir / "devices.json");
    auto d = devices.add("hermes-crypto (adopted)", Devices::default_scopes(), token, error);
    if (!d) { std::cerr << "ariadne: " << error << "\n"; return 1; }
    std::cout << "token adopted as " << d->id << ": phones paired with hermes-crypto keep working\n";
    return 0;
}

}  // namespace gateway
}  // namespace dd
