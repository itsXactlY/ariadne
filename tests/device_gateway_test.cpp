// tests/device_gateway_test.cpp — the device gateway: its crypto against vectors from an
// independent implementation (Python's `cryptography`, the same AES-GCM the app's
// GatewayCrypto and hermes-crypto's CryptoMiddleware use), the identity, the device
// store, every /command path, and one real TLS round trip through net::Server.

#include "dd/core/json.hpp"
#include "dd/gateway/crypto.hpp"
#include "dd/gateway/device_gateway.hpp"
#include "dd/gateway/devices.hpp"
#include "dd/gateway/identity.hpp"
#include "dd/gateway/relay.hpp"
#include "dd/net/server.hpp"

#include <curl/curl.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <thread>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace fs = std::filesystem;
namespace c = dd::gateway::crypto;
using dd::core::Json;
using dd::gateway::DeviceGateway;

static int g_fail = 0;
static void chk(bool ok, const std::string& what) {
    if (!ok) { ++g_fail; std::printf("FAIL %s\n", what.c_str()); }
}

static Json parse(const std::string& s) {
    try { return dd::core::json_parse(s); } catch (...) { return Json{}; }
}
static std::string str(const Json& o, const char* k) {
    const Json* v = dd::core::json_get(o, k);
    return v && v->type == Json::Str ? v->str : std::string();
}
static std::string enc_cmd(const std::string& key_raw, const std::string& sid, const std::string& cmd, const std::string& args) {
    Json inner = dd::core::json_obj();
    dd::core::obj_set(inner, "cmd", dd::core::json_str(cmd));
    dd::core::obj_set(inner, "args", dd::core::json_str(args));
    Json o = dd::core::json_obj();
    dd::core::obj_set(o, "session_id", dd::core::json_str(sid));
    dd::core::obj_set(o, "enc", dd::core::json_str(c::seal(key_raw, dd::core::json_dump(inner))));
    return dd::core::json_dump(o);
}
static Json open_reply(const std::string& key_raw, const dd::gateway::Reply& r) {
    const auto plain = c::open(key_raw, str(parse(r.body), "enc"));
    return plain ? parse(*plain) : Json{};
}

int main() {
    // ---- crypto: known answers ----
    {
        std::string key, nonce;
        for (int i = 0; i < 32; ++i) key += static_cast<char>(i);
        for (int i = 0; i < 16; ++i) nonce += static_cast<char>(i);
        const std::string pt = R"({"cmd":"pod","args":"{\"method\":\"GET\",\"path\":\"health\"}"})";
        const std::string want = "AAECAwQFBgcICQoLDA0OD9oz3CFaHmoFbj3M+0fqdWgcTsMbXcZwCu2Qvwt5hk3BM8BJm0X2j2EpWzm94Z2Q6FIjq7M4FIzsznUUJoBCjS51ShZ45HEh7dTV+jmcCT8=";
        chk(c::seal_with_nonce(key, nonce, pt) == want, "the envelope is byte for byte the one Python's AES-GCM makes (nonce16|tag16|ct)");
        chk(c::open(key, want).value_or("") == pt, "and it opens");
        std::string flipped = want;
        flipped[30] = flipped[30] == 'A' ? 'B' : 'A';
        chk(!c::open(key, flipped), "a flipped byte does not open");
        chk(!c::open(std::string(32, 'x'), want), "a wrong key does not open");
        const std::string s1 = c::seal(key, "x"), s2 = c::seal(key, "x");
        chk(s1 != s2 && c::open(key, s1).value_or("") == "x", "seal draws a fresh nonce each time");
    }
    {
        chk(c::hex(c::hmac_sha256("Jefe", "what do ya want for nothing?")) ==
                "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", "HMAC-SHA256, RFC 4231 case 2");
        std::string salt, info;
        for (int i = 0; i <= 0x0c; ++i) salt += static_cast<char>(i);
        for (int i = 0xf0; i <= 0xf9; ++i) info += static_cast<char>(i);
        chk(c::hex(c::hkdf_sha256(std::string(22, '\x0b'), salt, info, 42)) ==
                "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865", "HKDF-SHA256, RFC 5869 case 1");
        chk(c::unb64(c::b64(std::string("hello\0world", 11))).value_or("") == std::string("hello\0world", 11), "b64 round trip with a NUL");
        for (const std::string s : {"", "a", "ab", "abc", "abcd"}) chk(c::unb64(c::b64(s)).value_or("?") == s, "b64 round trip of '" + s + "'");
        chk(!c::unb64("abc"), "base64 of a bad length is refused");
    }
    {   // the e2e handshake: proof and wrap key match the Python reference
        const std::string token = "tok-0123456789";
        std::string nonce_raw;
        for (int i = 100; i < 116; ++i) nonce_raw += static_cast<char>(i);
        chk(c::b64(nonce_raw) == "ZGVmZ2hpamtsbW5vcHFycw==", "nonce vector");
        chk(DeviceGateway::e2e_proof(token, 1793300000, "ZGVmZ2hpamtsbW5vcHFycw==") == "fpJPbJ73O4qnC2YIQlEWf8ZOJnPAIJOwVcxcRtigIGM=",
            "e2e proof = HMAC(sha256(token), label|ts|nonce), as the app computes it");
        chk(c::hex(DeviceGateway::e2e_wrap_key(token, nonce_raw)) == "a5d7aaefa580b76d975b2a07fea6305dbc1f03c89237b148c3d813db5b8c900d",
            "e2e wrap key = HKDF(sha256(token), nonce, info)");
    }

    const fs::path dir = fs::temp_directory_path() / ("dd_gw_test_" + std::to_string(::getpid()));
    fs::remove_all(dir);

    // ---- identity ----
    std::string err;
    auto id = dd::gateway::load_or_create(dir, err);
    chk(id.has_value(), "a fresh home gets an identity: " + err);
    if (!id) { std::printf("%d FAILED\n", g_fail); return 1; }
    chk((fs::status(dir / "key.pem").permissions() & fs::perms::group_all) == fs::perms::none, "the key is not group- or world-readable");
    auto again = dd::gateway::load_or_create(dir, err);
    chk(again && again->fp_b64() == id->fp_b64(), "the second start loads the same identity (the phone's pin holds)");
    chk(c::unb64(id->fp_b64()).value_or("").size() == 32 && id->fp_hex().size() == 64, "fp is sha256: base64 for the pin, hex for the relay");
    chk(!id->cert_der().empty() && !dd::gateway::sign_sha256(*id, "nonce").empty(), "DER cert and a signature for the relay");
    chk(dd::gateway::RelayClient::phone_base("wss://relay.example/gw", *id) == "https://relay.example/gw/" + id->fp_hex(),
        "a phone posts to <relay>/gw/<fp hex>");
    const fs::path other = dir / "adopted";
    chk(dd::gateway::adopt(dir / "cert.pem", dir / "key.pem", other, err), "adopting another gateway's pair: " + err);
    chk(dd::gateway::load_or_create(other, err)->fp_b64() == id->fp_b64(), "an adopted pair keeps its fingerprint");
    chk(!dd::gateway::adopt(dir / "cert.pem", dir / "key.pem", other, err), "adopt never overwrites an identity");

    // ---- devices ----
    dd::gateway::Devices devices(dir / "devices.json");
    std::string tok_full, tok_chat;
    auto full = devices.add("full phone", {}, tok_full, err);
    auto chat = devices.add("chat only", {"chat"}, tok_chat, err);
    std::string bad_tok;
    chk(!devices.add("x", {"root"}, bad_tok, err), "an unknown scope is refused");
    chk(full && chat && tok_full.size() == 64, "devices are minted with a 32-byte token");
    chk(devices.by_token(tok_full)->id == full->id && !devices.by_token("nope"), "a token finds its device, nothing else does");
    chk(full->has("memory") && full->has("tools") && !chat->has("memory"), "default scopes chat,memory,tools; an explicit list is kept");

    // ---- /command ----
    dd::gateway::GatewayConfig cfg;
    cfg.pod_url = "http://127.0.0.1:1";   // nothing listens: the proxy answers with an error, inside the envelope
    cfg.bridge_url = "http://127.0.0.1:1";
    cfg.llm_url = "http://127.0.0.1:1";
    cfg.proxy_timeout_ms = 2000;
    DeviceGateway gw(cfg, devices);

    auto r = gw.command(R"({"cmd":"session"})", tok_full, false);
    Json created = *dd::core::json_get(parse(r.body), "created");
    const std::string sid = str(created, "session_id");
    const auto key = c::unb64(str(created, "client_key")).value_or("");
    chk(r.status == 200 && !sid.empty() && key.size() == 32, "v1 handshake: bearer -> session_id + client_key");
    chk(gw.command(R"({"cmd":"session"})", "wrong", false).status == 401, "a wrong bearer gets 401");
    chk(gw.command(R"({"cmd":"pod","args":"{}"})", "", false).status == 401, "a command without token or session gets 401");
    chk(gw.command(R"({"cmd":"status"})", "", false).status == 200, "status stays open, as on hermes-crypto");

    auto st = gw.command(enc_cmd(key, sid, "status", ""), "", false);
    chk(str(open_reply(key, st), "agent") == "ariadne" && str(open_reply(key, st), "device") == "full phone",
        "an encrypted command answers encrypted, naming the device");
    auto pod = open_reply(key, gw.command(enc_cmd(key, sid, "pod", R"({"method":"GET","path":"health"})"), "", false));
    chk(str(pod, "error").find("pod unreachable") == 0, "cmd:pod proxies (nothing on :1, so: unreachable)");
    auto bad = open_reply(key, gw.command(enc_cmd(key, sid, "pod", R"({"method":"GET","path":"../etc"})"), "", false));
    chk(str(bad, "error") == "pod: invalid path", "a traversal path is refused before any request");
    auto llm = open_reply(key, gw.command(enc_cmd(key, sid, "llm", R"({"method":"GET","path":"models"})"), "", false));
    chk(str(llm, "error") == "llm: invalid path", "cmd:llm only reaches v1/");
    auto llm2 = open_reply(key, gw.command(enc_cmd(key, sid, "llm", R"({"method":"POST","path":"v1/chat/completions","body":{"stream":true}})"), "", false));
    chk(str(llm2, "code") == "llm_down", "a stopped model says so: " + str(llm2, "error"));
    auto unk = open_reply(key, gw.command(enc_cmd(key, sid, "shell", "ls"), "", false));
    chk(str(unk, "code") == "unknown_command", "shell and the other hermes commands do not exist here");

    auto killed = open_reply(key, gw.command(enc_cmd(key, sid, "kill", ""), "", false));
    chk(str(killed, "destroyed") == sid, "kill names the session it destroyed");
    chk(gw.command(enc_cmd(key, sid, "status", ""), "", false).status == 400,
        "a killed session decrypts nothing, so the app renegotiates on Decryption failed");
    // The mark must outlive the session's TTL, not the FIFO's cap: the session is gone at
    // ttl, but a client that still holds the key can keep talking with it until the mark
    // expires too. With the old fixed-size FIFO the mark was dropped at ttl, so `kill`
    // answered "destroyed" while the session kept decrypting.
    cfg.session_ttl_s = 1;
    DeviceGateway short_ttl(cfg, devices);
    auto r3 = short_ttl.command(R"({"cmd":"session"})", tok_full, false);
    Json c3 = *dd::core::json_get(parse(r3.body), "created");
    const std::string sid3 = str(c3, "session_id");
    const auto key3 = c::unb64(str(c3, "client_key")).value_or("");
    chk(str(open_reply(key3, short_ttl.command(enc_cmd(key3, sid3, "kill", ""), "", false)), "destroyed") == sid3,
        "kill on a short-TTL gateway");
    std::this_thread::sleep_for(std::chrono::seconds(2));
    chk(short_ttl.command(enc_cmd(key3, sid3, "status", ""), "", false).status == 400,
        "the kill outlives the session TTL");

    auto r2 = gw.command(R"({"cmd":"session"})", tok_chat, false);
    Json c2 = *dd::core::json_get(parse(r2.body), "created");
    const auto key2 = c::unb64(str(c2, "client_key")).value_or("");
    auto denied = open_reply(key2, gw.command(enc_cmd(key2, str(c2, "session_id"), "pod", R"({"method":"GET","path":"health"})"), "", false));
    chk(str(denied, "code") == "scope", "a device without the memory scope cannot reach the pod");

    auto garbage = gw.command(enc_cmd(key, "not-a-session", "status", ""), "", false);
    chk(garbage.status == 400 && garbage.body.find("Decryption failed") != std::string::npos,
        "an unknown session answers 400 Decryption failed, which the app renegotiates on");
    chk(gw.command(enc_cmd(std::string(32, 'k'), sid, "status", ""), "", false).status == 400, "a wrong session key: 400 too");

    // ---- through the relay: no bearer, ever; the e2e handshake instead ----
    auto relayed = gw.command(R"({"cmd":"session"})", tok_full, true);
    chk(relayed.status == 401 && str(parse(relayed.body), "code") == "e2e_required", "via the relay a bearer session is refused");
    chk(gw.command(R"({"cmd":"pod","args":"{}"})", tok_full, true).status == 401, "via the relay a bearer command is refused");
    {
        const std::string nonce_raw = c::random_bytes(16), nonce = c::b64(nonce_raw);
        const long long ts = static_cast<long long>(std::time(nullptr));
        const std::string proof = DeviceGateway::e2e_proof(tok_full, ts, nonce);
        const std::string body = R"({"cmd":"session","e2e":{"ts":)" + std::to_string(ts) + R"(,"nonce":")" + nonce +
                                 R"(","proof":")" + proof + R"("}})";
        auto e = gw.command(body, "", true);
        Json ec = *dd::core::json_get(parse(e.body), "created");
        chk(e.status == 200 && str(ec, "client_key").empty() && !str(ec, "key_box").empty(),
            "e2e handshake: no client_key in clear, only a key_box");
        const auto ekey_b64 = c::open(DeviceGateway::e2e_wrap_key(tok_full, nonce_raw), str(ec, "key_box"));
        const auto ekey = c::unb64(ekey_b64.value_or("")).value_or("");
        chk(ekey.size() == 32, "the phone opens the key_box with what only it and the gateway hold");
        const auto replay = gw.command(body, "", true);
        chk(replay.status == 401 && str(parse(replay.body), "code") == "replay",
            "the same handshake answered twice is refused: a captured proof cannot mint a second session");
        auto est = open_reply(ekey, gw.command(enc_cmd(ekey, str(ec, "session_id"), "status", ""), "", true));
        chk(str(est, "device") == "full phone", "and drives encrypted commands through the relay");

        const std::string bad_proof = DeviceGateway::e2e_proof("not-the-token", ts, nonce);
        chk(gw.command(R"({"cmd":"session","e2e":{"ts":)" + std::to_string(ts) + R"(,"nonce":")" + nonce + R"(","proof":")" +
                           bad_proof + R"("}})", "", true).status == 401, "a proof from the wrong token is refused");
        const long long old = ts - 3600;
        chk(gw.command(R"({"cmd":"session","e2e":{"ts":)" + std::to_string(old) + R"(,"nonce":")" + nonce + R"(","proof":")" +
                           DeviceGateway::e2e_proof(tok_full, old, nonce) + R"("}})", "", true).status == 401,
            "a proof an hour old is refused");

        // revoke: the session dies with the device's row
        chk(devices.revoke(full->id, err), "revoke");
        chk(gw.command(enc_cmd(ekey, str(ec, "session_id"), "status", ""), "", true).status == 400,
            "a revoked device's open session is refused on its next request");
        chk(gw.command(R"({"cmd":"session"})", tok_full, false).status == 401, "and its token opens nothing new");
    }

    // ---- TLS through net::Server ----
    {
        std::string tok;
        devices.add("tls phone", {}, tok, err);
        void* ctx = dd::gateway::make_server_ctx(*id, err);
        chk(ctx != nullptr, "an SSL_CTX for the identity: " + err);
        dd::net::Server server("127.0.0.1", 0, [&](const dd::net::Request& q, dd::net::Conn& conn) {
            if (!gw.handle(q, conn)) conn.json(404, "{}");
        });
        server.set_tls(ctx);
        chk(server.listen(err), "listen: " + err);
        std::atomic<int> stop{0};
        std::thread t([&] { server.serve(stop); });
        curl_global_init(CURL_GLOBAL_DEFAULT);
        auto post = [&](const std::string& scheme, const std::string& path, const std::string& body, const std::string& bearer,
                        long& code) {
            std::string out;
            CURL* h = curl_easy_init();
            const std::string url = scheme + "://127.0.0.1:" + std::to_string(server.port()) + path;
            curl_slist* hd = curl_slist_append(nullptr, "Content-Type: application/json");
            if (!bearer.empty()) hd = curl_slist_append(hd, ("Authorization: Bearer " + bearer).c_str());
            curl_easy_setopt(h, CURLOPT_URL, url.c_str());
            curl_easy_setopt(h, CURLOPT_HTTPHEADER, hd);
            if (!body.empty()) curl_easy_setopt(h, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);   // the test pins by fp below, as the app does
            curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(h, CURLOPT_TIMEOUT, 10L);
            curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, +[](char* p, size_t s, size_t n, void* ud) {
                static_cast<std::string*>(ud)->append(p, s * n);
                return s * n;
            });
            curl_easy_setopt(h, CURLOPT_WRITEDATA, &out);
            const std::string pin = "sha256//" + id->fp_b64();
            if (scheme == "https") curl_easy_setopt(h, CURLOPT_PINNEDPUBLICKEY, pin.c_str());
            const CURLcode rc = curl_easy_perform(h);
            code = 0;
            curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
            curl_slist_free_all(hd);
            curl_easy_cleanup(h);
            return rc == CURLE_OK ? out : std::string("curl: ") + curl_easy_strerror(rc);
        };
        long code = 0;
        const std::string sess = post("https", "/command", R"({"cmd":"session"})", tok, code);
        chk(code == 200 && sess.find("client_key") != std::string::npos, "over TLS, pinned by fp: the v1 handshake works: " + sess);
        post("https", "/health", "", "", code);
        chk(code == 404 || code == 200, "GET routes answer over TLS");
        {   // the app's certificate fetch: handshake, then hang up without a request
            CURL* h = curl_easy_init();
            const std::string url = "https://127.0.0.1:" + std::to_string(server.port()) + "/";
            curl_easy_setopt(h, CURLOPT_URL, url.c_str());
            curl_easy_setopt(h, CURLOPT_CONNECT_ONLY, 1L);
            curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_perform(h);
            curl_easy_cleanup(h);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        post("https", "/command", R"({"cmd":"session"})", tok, code);
        chk(code == 200, "a peer that hangs up after its handshake does not take the server down (SIGPIPE)");
        const std::string plain = post("http", "/command", R"({"cmd":"session"})", tok, code);
        chk(code == 0, "plain HTTP on the TLS port gets no answer: " + plain);
        stop = 1;
        t.join();
        dd::gateway::free_server_ctx(ctx);
    }

    // ---- Mazemaker pairing: /pair and /pair.json, as hermes-crypto served them ----
    {
        auto call = [](const DeviceGateway& g, const std::string& path, const std::string& peer) {
            int sv[2];
            ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
            {
                dd::net::Conn conn(sv[0]);
                dd::net::Request q;
                q.method = "GET";
                q.path = path;
                q.peer = peer;
                g.handle(q, conn);
            }   // the Conn closes its end: the read below ends
            std::string out;
            char b[65536];
            ssize_t n;
            while ((n = ::read(sv[1], b, sizeof b)) > 0) out.append(b, static_cast<size_t>(n));
            ::close(sv[1]);
            return out;
        };
        auto body_of = [](const std::string& resp) {
            const size_t at = resp.find("\r\n\r\n");
            return at == std::string::npos ? std::string() : resp.substr(at + 4);
        };
        fs::create_directories(dir / "pair");
        dd::gateway::Devices pd(dir / "pair" / "devices.json");
        DeviceGateway g2(cfg, pd);
        chk(call(g2, "/pair.json", "127.0.0.1").find(" 503 ") != std::string::npos, "without set_pairing the page says it is not configured");
        g2.set_pairing(id->fp_b64(), "");

        const std::string r1 = call(g2, "/pair.json", "127.0.0.1");
        Json pj = parse(body_of(r1));
        const std::string code = str(pj, "code");
        chk(r1.rfind("HTTP/1.1 200", 0) == 0 && str(pj, "fp") == id->fp_b64(), "/pair.json answers {code, fp} on loopback");
        Json doc = parse(c::unb64(code).value_or(""));
        const Json* v = dd::core::json_get(doc, "v");
        chk(v && v->type == Json::Num && v->num == 1 && !str(doc, "token").empty() && str(doc, "fp") == id->fp_b64() &&
                !dd::core::json_get(doc, "host"),
            "the code is hermes-crypto's: base64({v:1, token, fp}), host-less (the app finds the gateway by mDNS)");
        chk(str(parse(body_of(call(g2, "/pair.json", "::1"))), "code") == code, "the code is stable: every scan gets the same token");
        chk(call(g2, "/pair.json", "192.168.0.5").find(" 403 ") != std::string::npos, "a LAN client gets 403: the code carries a token");
        chk(call(g2, "/pair", "192.168.0.5").find(" 403 ") != std::string::npos, "the page too");
        const std::string page = call(g2, "/pair", "127.0.0.1");
        chk(page.find("PAIR YOUR PHONE") != std::string::npos && page.find(code) != std::string::npos, "/pair is the page with the code");
        chk(page.find("<svg") != std::string::npos || page.find("install qrencode") != std::string::npos, "and a QR (or says how to get one)");
        chk(call(g2, "/", "127.0.0.1").find("PAIR YOUR PHONE") != std::string::npos, "/ is the page too, as on hermes-crypto");
        auto ps = g2.command(R"({"cmd":"session"})", str(doc, "token"), false);
        chk(ps.status == 200 && ps.body.find("client_key") != std::string::npos, "the scanned token opens a session");

        // adoption: hermes-crypto's gateway.token becomes the pairing token
        dd::gateway::Devices ad(dir / "adopt" / "devices.json");
        chk(ad.set_pairing_token("legacy-hermes-token", err) && ad.pairing_token(false, err).value_or("") == "legacy-hermes-token",
            "an adopted token is what /pair hands out");
        // revoking the pairing device ends that token; the next /pair mints another
        const auto pdev = pd.by_token(str(doc, "token"));
        chk(pdev && pd.revoke(pdev->id, err) && !pd.pairing_token(false, err), "a revoked pairing token is not handed out again");
        const std::string fresh = str(parse(c::unb64(str(parse(body_of(call(g2, "/pair.json", "127.0.0.1"))), "code")).value_or("")), "token");
        chk(!fresh.empty() && fresh != str(doc, "token"), "the next /pair mints a new pairing token");
    }
    {
    // ---- cmd:kill is a durable mark: the id must stop decrypting for good ----
        using Reply = dd::gateway::Reply;
        // `kill` by id, from the bearer path (the CLI and the relay's unsealed route reach
        // it without a session object). The mark has to outlive the request: without it the
        // id keeps decrypting until its expiry and the kill is a lie to the app.
        dd::gateway::Devices kd(dir / "kill" / "devices.json");
        std::string tok;
        const auto dev = kd.add("killer", {"chat"}, tok, err);
        if (!dev) { std::fprintf(stderr, "add failed: %s\n", err.c_str()); g_fail++; }
        DeviceGateway kg(cfg, kd);
        auto r = kg.command(R"({"cmd":"session"})", tok, false);
        Json created = *dd::core::json_get(parse(r.body), "created");
        const std::string sid = str(created, "session_id");
        const auto key = c::unb64(str(created, "client_key")).value_or("");
        chk(!sid.empty() && key.size() == 32, "a bearer session opens for the kill test");
        const auto alive = kg.command(enc_cmd(key, sid, "status", ""), "", false);
        chk(alive.status == 200, "the sealed session answers before the kill");
        const std::string kill_body = "{\"cmd\":\"kill\",\"session_id\":\"" + sid + "\"}";
        const Reply killed = kg.command(kill_body, tok, false);
        chk(killed.status == 200 && killed.body.find(sid) != std::string::npos,
            "kill names the id it destroyed");
        const auto after = kg.command(enc_cmd(key, sid, "status", ""), "", false);
        chk(after.status == 400 && after.body.find("Decryption failed") != std::string::npos,
            "the marked session decrypts nothing, even though it is still inside its expiry");
        // Killing the same id again is still a kill: the answer is the mark, not the agent.
        const Reply again = kg.command(kill_body, tok, false);
        chk(again.status == 200 && again.body.find("\"destroyed\"") != std::string::npos,
            "kill twice still reports the destroyed id");
        // A kill with no id marks nothing and must not pretend it did.
        const Reply bare = kg.command(R"({"cmd":"kill"})", tok, false);
        chk(bare.status == 200 && str(parse(bare.body), "destroyed").empty(),
            "kill with no session_id destroys nothing");
    }

    fs::remove_all(dir);
    if (g_fail) std::printf("%d FAILED\n", g_fail);
    else std::printf("gateway: all checks passed\n");
    return g_fail ? 1 : 0;
}
