// src/gateway/device_gateway.cpp — impl of dd/gateway/device_gateway.hpp.

#include "dd/gateway/device_gateway.hpp"

#include "dd/gateway/crypto.hpp"
#include "dd/net/http.hpp"
#include "dd/tools/url_safety.hpp"

#include <curl/curl.h>

#include <cctype>
#include <cstdlib>
#include <ctime>

namespace dd {
namespace gateway {

using core::Json;

namespace {

constexpr long kSkewS = 300;
constexpr const char* kProofLabel = "ariadne-e2e-session|";
constexpr const char* kWrapInfo = "ariadne-e2e-session-key";

Json jerr(const std::string& msg, const char* code = nullptr) {
    Json o = core::json_obj();
    core::obj_set(o, "error", core::json_str(msg));
    if (code) core::obj_set(o, "code", core::json_str(code));
    return o;
}

Reply reply(int status, const Json& j) { return Reply{status, core::json_dump(j)}; }

std::string str_of(const Json* v) { return v && v->type == Json::Str ? v->str : std::string(); }

long long now_s() { return static_cast<long long>(std::time(nullptr)); }

bool loopback_host(const std::string& url) {
    const std::string h = tools::url_hostname(url);
    return h == "127.0.0.1" || h == "localhost" || h == "::1";
}

// Where a scope is needed and missing, the answer names both.
std::optional<Json> need(const Device& d, const char* scope) {
    if (d.has(scope)) return std::nullopt;
    return jerr("device '" + d.name + "' was not paired for '" + scope + "'", "scope");
}

}  // namespace

std::string bearer_of(const net::Request& req) {
    const std::string a = req.header("authorization");
    if (a.size() > 7 && (a.compare(0, 7, "Bearer ") == 0 || a.compare(0, 7, "bearer ") == 0)) {
        size_t s = 7;
        while (s < a.size() && a[s] == ' ') ++s;
        return a.substr(s);
    }
    return {};
}

DeviceGateway::DeviceGateway(GatewayConfig cfg, Devices devices)
    : cfg_(std::move(cfg)), devices_(std::move(devices)), secret_(crypto::random_bytes(32)) {
    // These are the operator's own servers, named in the config: url_safety admits them by name.
    for (const auto* u : {&cfg_.pod_url, &cfg_.bridge_url, &cfg_.llm_url})
        if (!u->empty()) tools::admit_configured_url(*u);
}

std::string DeviceGateway::e2e_proof(const std::string& token, long long ts, const std::string& nonce_b64) {
    return crypto::b64(crypto::hmac_sha256(crypto::sha256(token), kProofLabel + std::to_string(ts) + "|" + nonce_b64));
}

std::string DeviceGateway::e2e_wrap_key(const std::string& token, const std::string& nonce_raw) {
    return crypto::hkdf_sha256(crypto::sha256(token), nonce_raw, kWrapInfo, 32);
}

std::string DeviceGateway::open_session(const Device& d, std::string& key_out) const {
    key_out = crypto::random_bytes(32);
    Json s = core::json_obj();
    core::obj_set(s, "d", core::json_str(d.id));
    core::obj_set(s, "k", core::json_str(crypto::b64(key_out)));
    core::obj_set(s, "x", core::json_num(static_cast<double>(now_s() + cfg_.session_ttl_s)));
    return crypto::seal(secret_, core::json_dump(s));
}

std::optional<DeviceGateway::Session> DeviceGateway::resume(const std::string& session_id) const {
    const auto plain = crypto::open(secret_, session_id);
    if (!plain) return std::nullopt;
    Json s;
    try { s = core::json_parse(*plain); } catch (const std::exception&) { return std::nullopt; }
    const Json* x = core::json_get(s, "x");
    if (!x || x->type != Json::Num || static_cast<long long>(x->num) < now_s()) return std::nullopt;
    auto d = devices_.by_id(str_of(core::json_get(s, "d")));   // revoked since: the session dies with the row
    auto key = crypto::unb64(str_of(core::json_get(s, "k")));
    if (!d || !key || key->size() != 32) return std::nullopt;
    return Session{std::move(*d), std::move(*key)};
}

Reply DeviceGateway::command(const std::string& body, const std::string& bearer, bool via_relay) const {
    Json data;
    try { data = core::json_parse(body); } catch (const std::exception&) { return reply(400, jerr("Invalid JSON")); }
    if (data.type != Json::Obj) return reply(400, jerr("Invalid JSON"));

    const std::string sid = str_of(core::json_get(data, "session_id"));
    std::optional<Session> sess;
    Json inner = data;
    if (const Json* enc = core::json_get(data, "enc"); enc && enc->type == Json::Str) {
        sess = resume(sid);
        // "Decryption failed" is what the app renegotiates on: an expired session, a
        // session from before a restart and a wrong key all land here, on purpose.
        if (!sess) return reply(400, jerr("Decryption failed: unknown or expired session"));
        const auto plain = crypto::open(sess->key, enc->str);
        if (!plain) return reply(400, jerr("Decryption failed"));
        try { inner = core::json_parse(*plain); } catch (const std::exception&) { return reply(400, jerr("Decryption failed")); }
        if (inner.type != Json::Obj) return reply(400, jerr("Decryption failed"));
    }

    const std::string cmd = str_of(core::json_get(inner, "cmd"));
    if (cmd.empty()) return reply(400, jerr("No 'cmd' field"));

    if (cmd == "status" || cmd == "health") {   // open, as on hermes-crypto: liveness for anyone
        Json o = core::json_obj();
        core::obj_set(o, "status", core::json_str("ok"));
        core::obj_set(o, "gateway", core::json_str("running"));
        core::obj_set(o, "agent", core::json_str("ariadne"));
        core::obj_set(o, "local_crypto", core::json_str("AES256-GCM"));
        if (sess) core::obj_set(o, "device", core::json_str(sess->device.name));
        if (!sess) return reply(200, o);
        return reply(200, [&] { Json e = core::json_obj(); core::obj_set(e, "enc", core::json_str(crypto::seal(sess->key, core::json_dump(o)))); return e; }());
    }

    if (cmd == "session" && !sess) {
        std::string key;
        Json created = core::json_obj();
        if (const Json* e2e = core::json_get(inner, "e2e"); e2e && e2e->type == Json::Obj) {
            const Json* ts = core::json_get(*e2e, "ts");
            const std::string nonce = str_of(core::json_get(*e2e, "nonce"));
            const auto proof = crypto::unb64(str_of(core::json_get(*e2e, "proof")));
            const auto nonce_raw = crypto::unb64(nonce);
            if (!ts || ts->type != Json::Num || !proof || !nonce_raw || nonce_raw->size() < 16)
                return reply(400, jerr("e2e: ts, nonce (16+ bytes) and proof are required"));
            const long long t = static_cast<long long>(ts->num);
            if (std::llabs(t - now_s()) > kSkewS) return reply(401, jerr("e2e: the clocks differ by more than five minutes", "skew"));
            std::optional<Device> who;
            std::string token_hash;
            for (auto& d : devices_.list()) {
                const auto th = crypto::unhex(d.token_sha256);
                if (!th) continue;
                const std::string want = crypto::hmac_sha256(*th, kProofLabel + std::to_string(t) + "|" + nonce);
                if (crypto::equal(want, *proof) && !who) { who = d; token_hash = *th; }
            }
            if (!who) return reply(401, jerr("Unauthorized: no paired device made that proof"));
            const std::string sid_new = open_session(*who, key);
            // The wrap key is derived from sha256(token) -- what both ends hold -- and the
            // client's nonce, so the relay sees a box it has no key for.
            const std::string wrap = crypto::hkdf_sha256(token_hash, *nonce_raw, kWrapInfo, 32);
            core::obj_set(created, "session_id", core::json_str(sid_new));
            core::obj_set(created, "key_box", core::json_str(crypto::seal(wrap, crypto::b64(key))));
            core::obj_set(created, "e2e", core::json_num(1));
        } else {
            if (via_relay)
                return reply(401, jerr("Unauthorized: through the relay a session opens with the e2e handshake; "
                                       "the bearer token never crosses it", "e2e_required"));
            auto who = devices_.by_token(bearer);
            if (!who) return reply(401, jerr("Unauthorized: this command requires a gateway token"));
            core::obj_set(created, "session_id", core::json_str(open_session(*who, key)));
            core::obj_set(created, "client_key", core::json_str(crypto::b64(key)));
        }
        Json o = core::json_obj();
        core::obj_set(o, "created", created);
        return reply(200, o);
    }

    // Everything else needs a device: the session's, or (on the LAN only) the bearer's.
    std::optional<Device> who;
    if (sess) who = sess->device;
    else if (!via_relay) who = devices_.by_token(bearer);
    if (!who) return reply(401, jerr("Unauthorized: this command requires a gateway token"));

    Json args = core::json_obj();
    if (const Json* a = core::json_get(inner, "args")) {
        if (a->type == Json::Str && !a->str.empty()) {
            try { args = core::json_parse(a->str); } catch (const std::exception&) { args = core::json_str(a->str); }
        } else if (a->type == Json::Obj) {
            args = *a;
        }
    }
    const Json result = dispatch(cmd, args, *who, sid);
    if (!sess) return reply(200, result);
    Json o = core::json_obj();
    core::obj_set(o, "enc", core::json_str(crypto::seal(sess->key, core::json_dump(result))));
    return reply(200, o);
}

Json DeviceGateway::dispatch(const std::string& cmd, const Json& args, const Device& d, const std::string& session_id) const {
    if (cmd == "pod") {
        if (auto e = need(d, "memory")) return *e;
        return proxy(cfg_.pod_url, "pod", args, "", "", false);
    }
    if (cmd == "bridge") {
        if (auto e = need(d, "chat")) return *e;
        return proxy(cfg_.bridge_url, "bridge", args, "", "", false);
    }
    if (cmd == "llm") {
        if (auto e = need(d, "chat")) return *e;
        // Plaintext to a model is acceptable on this machine only.
        if (!loopback_host(cfg_.llm_url))
            return jerr("llm: the gateway's model must be on this machine (loopback); " + cfg_.llm_url + " is not", "llm_not_local");
        const std::string where = cfg_.llm_url.substr(cfg_.llm_url.find("://") == std::string::npos ? 0 : cfg_.llm_url.find("://") + 3);
        return proxy(cfg_.llm_url, "llm", args, "v1/",
                     "the local model on the desktop is not running — nothing answers on " + where, true);
    }
    if (cmd == "sessions") {
        Json o = core::json_obj();
        core::obj_set(o, "sessions", core::json_arr());   // sessions are sealed ids, not rows
        return o;
    }
    if (cmd == "kill") {
        Json o = core::json_obj();
        core::obj_set(o, "destroyed", core::json_str(session_id));
        return o;
    }
    return jerr("unknown command '" + cmd + "' (this gateway runs session, status, pod, bridge, llm)", "unknown_command");
}

Json DeviceGateway::proxy(const std::string& base, const char* label, const Json& args, const char* path_prefix,
                          const std::string& down_message, bool force_no_stream) const {
    if (args.type != Json::Obj) return jerr(std::string(label) + ": args must be a JSON object {method,path,body}");
    std::string method = str_of(core::json_get(args, "method"));
    for (char& c : method) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (method.empty()) method = "GET";
    std::string path = str_of(core::json_get(args, "path"));
    while (!path.empty() && path.front() == '/') path.erase(0, 1);
    if (path.empty() || path.find("://") != std::string::npos || path.find("..") != std::string::npos ||
        path.rfind(path_prefix, 0) != 0)
        return jerr(std::string(label) + ": invalid path");
    if (method != "GET" && method != "POST") return jerr(std::string(label) + ": only GET and POST");
    const std::string url = base + "/" + path;
    net::Response r;
    if (method == "POST") {
        Json body = core::json_obj();
        if (const Json* b = core::json_get(args, "body")) body = core::json_deep_copy(*b);
        if (force_no_stream && body.type == Json::Obj) core::obj_set(body, "stream", core::json_bool(false));
        r = net::post(url, core::json_dump(body), cfg_.proxy_timeout_ms);
    } else {
        r = net::get(url, cfg_.proxy_timeout_ms);
    }
    if (!r.error.empty()) {
        if (!down_message.empty()) {
            Json o = jerr(down_message, (std::string(label) + "_down").c_str());
            core::obj_set(o, "detail", core::json_str(r.error.substr(0, 200)));
            return o;
        }
        return jerr(std::string(label) + " unreachable: " + r.error);
    }
    if (r.status < 200 || r.status >= 300) {
        Json o = jerr(std::string(label) + " HTTP " + std::to_string(r.status));
        core::obj_set(o, "detail", core::json_str(r.body.substr(0, 500)));
        return o;
    }
    try {
        return core::json_parse(r.body);
    } catch (const std::exception&) {
        Json o = core::json_obj();
        core::obj_set(o, "raw", core::json_str(r.body.substr(0, 4000)));
        return o;
    }
}

std::optional<Device> DeviceGateway::device(const net::Request& req) const { return devices_.by_token(bearer_of(req)); }

namespace {

struct StreamSink {
    net::Conn* conn;
    bool started = false;
};

size_t stream_write(char* p, size_t size, size_t n, void* ud) {
    auto* s = static_cast<StreamSink*>(ud);
    if (!s->started) {
        s->started = true;
        if (!s->conn->stream_begin(200, "text/event-stream")) return 0;
    }
    return s->conn->stream_write(std::string_view(p, size * n)) ? size * n : 0;   // 0 aborts: the phone left
}

}  // namespace

void DeviceGateway::bridge_stream(const net::Request& req, net::Conn& conn) const {
    CURL* c = curl_easy_init();
    if (!c) { conn.json(502, core::json_dump(jerr("bridge: no curl handle"))); return; }
    const std::string url = cfg_.bridge_url + "/chat/stream";
    curl_slist* h = curl_slist_append(nullptr, "Content-Type: application/json");
    h = curl_slist_append(h, "Accept: text/event-stream");
    StreamSink sink{&conn};
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, req.body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, stream_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, cfg_.proxy_timeout_ms);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    const CURLcode rc = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    if (!sink.started) {   // nothing reached the phone yet: answer in JSON instead of an empty stream
        Json e = rc == CURLE_OK ? jerr("bridge HTTP " + std::to_string(status)) : jerr(std::string("bridge unreachable: ") + curl_easy_strerror(rc));
        conn.json(502, core::json_dump(e));
    }
}

bool DeviceGateway::handle(const net::Request& req, net::Conn& conn) const {
    if (req.path == "/health" && req.method == "GET") {
        conn.json(200, R"({"status":"ok","gateway":"running","agent":"ariadne"})");
        return true;
    }
    if (req.path == "/command" && req.method == "POST") {
        const Reply r = command(req.body, bearer_of(req), false);
        conn.json(r.status, r.body);
        return true;
    }
    if (req.path == "/bridge/stream" && req.method == "POST") {
        // Per-chunk AES does not fit SSE: this rides TLS + token, on the LAN only.
        auto who = devices_.by_token(bearer_of(req));
        if (!who) { conn.json(401, R"({"error":"Unauthorized"})"); return true; }
        if (auto e = need(*who, "chat")) { conn.json(403, core::json_dump(*e)); return true; }
        bridge_stream(req, conn);
        return true;
    }
    return false;
}

}  // namespace gateway
}  // namespace dd
