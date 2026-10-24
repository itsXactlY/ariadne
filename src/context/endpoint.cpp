// src/context/endpoint.cpp — impl of dd/context/endpoint.hpp.
#include "dd/context/endpoint.hpp"

#include "dd/net/http.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <thread>

namespace dd {
namespace context {

using core::Json;

namespace {

const char* const kLadder[] = {"off", "low", "medium", "high", "xhigh", "max", "ultra"};

double num(const Json& o, const char* k) {
    const Json* v = core::json_get(o, k);
    return v && v->type == Json::Num ? v->num : 0.0;
}

// scheme://[user@]host[:port]/path -> host, lowercased, brackets off an IPv6 literal. "" when there is none.
std::string url_host(const std::string& url) {
    size_t b = url.find("://");
    b = b == std::string::npos ? 0 : b + 3;
    size_t e = url.find_first_of("/?#", b);
    std::string auth = url.substr(b, e == std::string::npos ? std::string::npos : e - b);
    if (const size_t at = auth.rfind('@'); at != std::string::npos) auth.erase(0, at + 1);
    std::string host;
    if (!auth.empty() && auth[0] == '[') {
        const size_t close = auth.find(']');
        host = auth.substr(1, close == std::string::npos ? std::string::npos : close - 1);
    } else {
        host = auth.substr(0, auth.find(':'));
    }
    for (auto& c : host) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return host;
}

bool ends_with(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// What a llama-server or Strata /props carries; false: this is some other server.
bool answers_like_llama(const net::Response& p, ServerInfo& out) {
    if (!p.ok()) return false;
    try {
        const Json j = core::json_parse(p.body);
        out.reachable = true;
        if (const Json* g = core::json_get(j, "default_generation_settings"); g && g->type == Json::Obj)
            out.n_ctx = static_cast<long>(num(*g, "n_ctx"));
        if (out.n_ctx <= 0) out.n_ctx = static_cast<long>(num(j, "n_ctx"));
        if (const Json* t = core::json_get(j, "chat_template"); t && t->type == Json::Str) out.chat_template = t->str;
        if (const Json* bi = core::json_get(j, "build_info"); bi && bi->type == Json::Str) out.build = bi->str;
        out.strata = out.build.rfind("Strata", 0) == 0;
        if (const Json* ts = core::json_get(j, "total_slots"); ts && ts->type == Json::Num && ts->num >= 1)
            out.slots = static_cast<int>(ts->num);
    } catch (...) {
        return false;
    }
    return out.n_ctx > 0 || !out.build.empty();
}

}  // namespace

HostKind classify_host(const std::string& base_url) {
    const std::string host = url_host(base_url);
    if (host.empty()) return HostKind::Remote;
    if (host == "localhost" || ends_with(host, ".localhost")) return HostKind::Loopback;
    if (ends_with(host, ".local")) return HostKind::Private;
    in_addr a4{};
    if (::inet_pton(AF_INET, host.c_str(), &a4) == 1) {
        const uint32_t ip = ntohl(a4.s_addr);
        if ((ip >> 24) == 127) return HostKind::Loopback;
        if ((ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8 || (ip >> 16) == 0xA9FE) return HostKind::Private;
        return HostKind::Remote;
    }
    in6_addr a6{};
    if (::inet_pton(AF_INET6, host.c_str(), &a6) == 1) {
        static const unsigned char kLoop[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        if (std::memcmp(a6.s6_addr, kLoop, 16) == 0) return HostKind::Loopback;
        if ((a6.s6_addr[0] & 0xFE) == 0xFC) return HostKind::Private;   // fc00::/7
    }
    return HostKind::Remote;
}

Endpoint::Endpoint(std::string base_url) : base_url_(std::move(base_url)) {
    http_get = [](const std::string& url, long timeout_ms) { return net::get(url, timeout_ms); };
    std::string b = base_url_;
    while (!b.empty() && b.back() == '/') b.pop_back();
    if (b.size() >= 3 && b.compare(b.size() - 3, 3, "/v1") == 0) b.resize(b.size() - 3);
    switch (classify_host(base_url_)) {
        case HostKind::Loopback: root_ = b; break;
        case HostKind::Private: candidate_ = b; break;
        case HostKind::Remote: break;
    }
    info_.local = !root_.empty();
}

const ServerInfo& Endpoint::probe() {
    if (root_.empty() && candidate_.empty()) return info_;
    if (root_.empty()) {   // a LAN box is a llama-server only when it says so
        ServerInfo found;
        if (!answers_like_llama(http_get(candidate_ + "/props", 3000), found)) return info_;
        info_ = found;
        root_ = candidate_;
        info_.local = true;
    } else {
        answers_like_llama(http_get(root_ + "/props", 3000), info_);
    }
    if (const auto s = slots(); !s.empty()) info_.slots = static_cast<int>(s.size());
    main_ = std::min(main_.load(), std::max(0, info_.slots - 1));
    return info_;
}

int Endpoint::main_slot() const { return std::min(main_.load(), std::max(0, info_.slots - 1)); }

int Endpoint::sidekick_slot() const {
    if (info_.slots < 2) return 0;
    return main_slot() != 0 ? 0 : 1;
}

void Endpoint::promote(int slot) {
    if (slot >= 0 && slot < info_.slots) main_ = slot;
}

std::vector<Endpoint::Slot> Endpoint::slots() const {
    std::vector<Slot> out;
    if (root_.empty()) return out;
    const net::Response r = http_get(root_ + "/slots", 1500);
    if (!r.ok()) return out;
    try {
        const Json j = core::json_parse(r.body);
        if (j.type != Json::Arr) return out;
        for (const auto& sp : j.arr) {
            if (!sp || sp->type != Json::Obj) continue;
            Slot s;
            s.id = static_cast<int>(num(*sp, "id"));
            s.tokens = static_cast<long>(num(*sp, "n_prompt_tokens"));
            if (s.tokens <= 0) s.tokens = static_cast<long>(num(*sp, "n_past"));
            if (const Json* b = core::json_get(*sp, "is_processing"); b && b->type == Json::Bool) s.busy = b->b;
            out.push_back(s);
        }
    } catch (...) {
    }
    return out;
}

long Endpoint::pool_tokens(long main_tokens) const {
    if (!multi_slot() || info_.strata) return main_tokens;  // strata: slots are separate sessions, one shared pool never exists
    const int side = sidekick_slot();
    for (const auto& s : slots())
        if (s.id == side) return main_tokens + s.tokens;
    return main_tokens;
}

bool Endpoint::erase(int slot) const {
    if (root_.empty()) return false;
    return net::post(root_ + "/slots/" + std::to_string(slot) + "?action=erase", "{}", 5000).ok();
}

void Endpoint::drop_parked() const {
    if (!single_sequence()) return;
    std::thread([url = root_ + "/v1/cache/drop_parked"] { net::post(url, "{}", 35000); }).detach();
}

void Endpoint::shape(Json& body, int slot, bool thinking) const {
    if (root_.empty()) return;   // a remote API gets the OpenAI fields only
    core::obj_set(body, "n_cache_reuse", core::json_num(256));   // a spliced middle is relocated, not re-read
    if (!single_sequence() && slot >= 0) core::obj_set(body, "id_slot", core::json_num(slot));
    if (thinking && thinking_budget > 0) {
        // llama-server reads it as thinking_budget_tokens; Strata knows only this name
        core::obj_set(body, "reasoning_budget_tokens", core::json_num(thinking_budget));
        core::obj_set(body, "reasoning_budget_message", core::json_str(kThinkingBudgetMessage));
    }
}

std::vector<std::string> Endpoint::efforts() const {
    std::vector<std::string> out;
    const std::string& t = info_.chat_template;
    if (!info_.reachable || t.empty()) {
        for (const char* e : kLadder) out.emplace_back(e);
        return out;
    }
    const bool levels = t.find("reasoning_effort") != std::string::npos;
    if (!levels && t.find("enable_thinking") == std::string::npos) return out;   // no switch at all
    out.emplace_back("off");
    for (int i = 1; i < 7; ++i) {
        const std::string e = kLadder[i];
        if (!levels ? i <= 3 : t.find("'" + e + "'") != std::string::npos || t.find("\"" + e + "\"") != std::string::npos)
            out.push_back(e);
    }
    return out;
}

}  // namespace context
}  // namespace dd
