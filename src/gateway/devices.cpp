// src/gateway/devices.cpp — impl of dd/gateway/devices.hpp.

#include "dd/gateway/devices.hpp"

#include "dd/core/json.hpp"
#include "dd/gateway/crypto.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <sstream>

namespace dd {
namespace gateway {

namespace fs = std::filesystem;
using core::Json;

bool Device::has(std::string_view scope) const {
    return std::find(scopes.begin(), scopes.end(), scope) != scopes.end();
}

const std::vector<std::string>& Devices::default_scopes() {
    static const std::vector<std::string> s{"chat", "memory", "tools"};
    return s;
}

bool Devices::known_scope(std::string_view scope) {
    return scope == "chat" || scope == "memory" || scope == "tools" || scope == "admin";
}

std::vector<Device> Devices::list() const {
    std::vector<Device> out;
    std::ifstream f(file_);
    if (!f) return out;
    std::ostringstream s;
    s << f.rdbuf();
    Json doc;
    try { doc = core::json_parse(s.str()); } catch (const std::exception&) { return out; }
    const Json* arr = core::json_get(doc, "devices");
    if (!arr || arr->type != Json::Arr) return out;
    for (const auto& e : arr->arr) {
        if (!e || e->type != Json::Obj) continue;
        auto str = [&](const char* k) {
            const Json* v = core::json_get(*e, k);
            return v && v->type == Json::Str ? v->str : std::string();
        };
        Device d;
        d.id = str("id");
        d.name = str("name");
        d.token_sha256 = str("token_sha256");
        if (const Json* c = core::json_get(*e, "created"); c && c->type == Json::Num) d.created = static_cast<long long>(c->num);
        if (const Json* sc = core::json_get(*e, "scopes"); sc && sc->type == Json::Arr)
            for (const auto& x : sc->arr)
                if (x && x->type == Json::Str) d.scopes.push_back(x->str);
        if (!d.id.empty() && d.token_sha256.size() == 64) out.push_back(std::move(d));
    }
    return out;
}

std::optional<Device> Devices::by_token(std::string_view token) const {
    if (token.empty()) return std::nullopt;
    const std::string want = crypto::hex(crypto::sha256(token));
    std::optional<Device> hit;
    for (auto& d : list())
        if (crypto::equal(d.token_sha256, want) && !hit) hit = d;   // compare every row: no early exit to time
    return hit;
}

std::optional<Device> Devices::by_id(std::string_view id) const {
    for (auto& d : list())
        if (d.id == id) return d;
    return std::nullopt;
}

bool Devices::save(const std::vector<Device>& all, std::string& error) const {
    Json arr = core::json_arr();
    for (const auto& d : all) {
        Json sc = core::json_arr();
        for (const auto& s : d.scopes) core::arr_push(sc, core::json_str(s));
        Json o = core::json_obj();
        core::obj_set(o, "id", core::json_str(d.id));
        core::obj_set(o, "name", core::json_str(d.name));
        core::obj_set(o, "token_sha256", core::json_str(d.token_sha256));
        core::obj_set(o, "scopes", sc);
        core::obj_set(o, "created", core::json_num(static_cast<double>(d.created)));
        core::arr_push(arr, o);
    }
    Json doc = core::json_obj();
    core::obj_set(doc, "devices", arr);
    std::error_code ec;
    fs::create_directories(file_.parent_path(), ec);
    ::chmod(file_.parent_path().c_str(), 0700);
    const fs::path tmp = file_.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) { error = "cannot write " + tmp.string(); return false; }
        f << core::json_dump(doc) << "\n";
        if (!f) { error = "cannot write " + tmp.string(); return false; }
    }
    ::chmod(tmp.c_str(), 0600);
    fs::rename(tmp, file_, ec);
    if (ec) { error = "cannot place " + file_.string() + ": " + ec.message(); return false; }
    return true;
}

std::optional<Device> Devices::add(const std::string& name, const std::vector<std::string>& scopes, std::string& token,
                                   std::string& error) {
    for (const auto& s : scopes)
        if (!known_scope(s)) { error = "unknown scope '" + s + "' (chat, memory, tools, admin)"; return std::nullopt; }
    if (token.empty()) token = crypto::hex(crypto::random_bytes(32));
    auto all = list();
    const std::string hash = crypto::hex(crypto::sha256(token));
    for (const auto& d : all)
        if (d.token_sha256 == hash) { error = "that token already belongs to device " + d.id; return std::nullopt; }
    Device d;
    d.id = "dev-" + crypto::hex(crypto::random_bytes(4));
    d.name = name.empty() ? d.id : name;
    d.token_sha256 = hash;
    d.scopes = scopes.empty() ? default_scopes() : scopes;
    d.created = static_cast<long long>(std::time(nullptr));
    all.push_back(d);
    if (!save(all, error)) return std::nullopt;
    return d;
}

namespace {
constexpr const char* kPairingName = "mazemaker pairing";
}

std::optional<std::string> Devices::pairing_token(bool create, std::string& error) {
    {
        std::ifstream f(pairing_file());
        std::string token;
        std::getline(f, token);
        while (!token.empty() && (token.back() == '\r' || token.back() == ' ')) token.pop_back();
        if (!token.empty()) {
            if (by_token(token)) return token;
            // The file outlived its row (revoked): it is no longer a pairing token.
            if (!create) { error = "the pairing token was revoked; a new one is minted on the next /pair"; return std::nullopt; }
        } else if (!create) {
            error = "no pairing token yet";
            return std::nullopt;
        }
    }
    std::string token;
    if (!add(kPairingName, default_scopes(), token, error)) return std::nullopt;
    if (!set_pairing_token(token, error)) return std::nullopt;
    return token;
}

bool Devices::set_pairing_token(const std::string& token, std::string& error) {
    if (!by_token(token)) {
        std::string t = token;
        if (!add(kPairingName, default_scopes(), t, error)) return false;
    }
    std::error_code ec;
    fs::create_directories(pairing_file().parent_path(), ec);
    const fs::path tmp = pairing_file().string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) { error = "cannot write " + tmp.string(); return false; }
        f << token << "\n";
        if (!f) { error = "cannot write " + tmp.string(); return false; }
    }
    ::chmod(tmp.c_str(), 0600);
    fs::rename(tmp, pairing_file(), ec);
    if (ec) { error = "cannot place " + pairing_file().string() + ": " + ec.message(); return false; }
    return true;
}

bool Devices::revoke(std::string_view id, std::string& error) {
    auto all = list();
    const auto it = std::remove_if(all.begin(), all.end(), [&](const Device& d) { return d.id == id; });
    if (it == all.end()) { error = "no device " + std::string(id); return false; }
    all.erase(it, all.end());
    return save(all, error);
}

}  // namespace gateway
}  // namespace dd
