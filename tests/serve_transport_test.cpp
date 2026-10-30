// What a request's transport decides: `net::Request::tls` and `.peer` are computed by the
// server, and ChatApi is the surface that has to act on them. A bearer token that arrives over
// plain HTTP from off-loopback would cross the wire unread, so that request is refused (403)
// before the token is ever compared. Loopback cleartext is a local client or a proxy that
// terminated TLS in front, and is answered; TLS from anywhere is answered; /health answers
// regardless, because a liveness probe carries nothing worth hiding.
//
// The requests are built by hand rather than over a socket: a loopback socketpair cannot
// pretend to be a remote peer, and the fact under test is the peer/tls pair itself.
#include "dd/net/server.hpp"
#include "dd/serve/chat_api.hpp"
#include "dd/core/json.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <string>

using namespace dd;

static int failures = 0;
#define CHECK(cond, msg)                                   \
    do {                                                   \
        if (!(cond)) {                                     \
            std::cerr << "FAIL: " << msg << "\n";          \
            ++failures;                                    \
        }                                                  \
    } while (0)

struct NullRunner : serve::TurnRunner {
    std::string run(const std::string&) override { return "pong"; }
    void interrupt() override {}
};

// One request through ChatApi, and the bytes the client would read back.
static std::string ask(const std::string& key, const net::Request& req) {
    NullRunner runner;
    serve::ChatApi api(serve::ApiInfo{"m", "upstream", "/home", 3}, key, runner);
    int sp[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) return "";
    {
        // The Conn closes its fd when it dies; reading first would wait on a socket nobody
        // is writing to any more.
        net::Conn conn(sp[0]);
        api.handle(req, conn);
    }
    std::string out;
    char buf[2048];
    for (;;) {
        const ssize_t n = ::recv(sp[1], buf, sizeof buf, 0);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    ::close(sp[0]);
    ::close(sp[1]);
    return out;
}

static net::Request request(const std::string& path, const std::string& peer, bool tls,
                            const std::string& auth) {
    net::Request r;
    r.method = "GET";
    r.path = path;
    r.target = path;
    r.peer = peer;
    r.tls = tls;
    if (!auth.empty()) r.headers.emplace_back("authorization", "Bearer " + auth);
    return r;
}

static int status_of(const std::string& resp) {
    if (resp.rfind("HTTP/1.1 ", 0) != 0) return 0;
    return std::atoi(resp.c_str() + 9);
}

int main() {
    const std::string key = "sekret";

    // A key set on a plain-HTTP listener, from a remote peer: refused, and the token is not
    // even compared — the answer says why and how to serve it safely.
    std::string r = ask(key, request("/v1/models", "198.51.100.7", false, key));
    CHECK(status_of(r) == 403, "cleartext off-loopback with a correct token must not be answered, got " + std::to_string(status_of(r)));
    CHECK(r.find("transport_error") != std::string::npos, "refusal must name the transport, not auth");
    CHECK(r.find("plain HTTP") != std::string::npos, "refusal must say the listener is plain HTTP");
    CHECK(r.find("127.0.0.1") != std::string::npos, "refusal must name the proxy-forwarded-loopback way out");

    // A wrong token on the same cleartext remote connection is still refused on transport:
    // answering 401 here would confirm the listener is reachable and worth sniffing.
    CHECK(status_of(ask(key, request("/v1/models", "198.51.100.7", false, "wrong"))) == 403,
          "cleartext off-loopback must refuse before auth is compared");

    // TLS from the same remote peer is answered: the token cannot be read in transit.
    CHECK(status_of(ask(key, request("/v1/models", "198.51.100.7", true, key))) == 200,
          "TLS off-loopback with the token must be answered");
    CHECK(status_of(ask(key, request("/v1/models", "198.51.100.7", true, "wrong"))) == 401,
          "TLS off-loopback with a wrong token must be a 401");

    // Loopback cleartext is answered: a local client, or a proxy that terminated TLS in front
    // and forwards to 127.0.0.1. This is the deployment a plain --serve public bind relies on.
    CHECK(status_of(ask(key, request("/v1/models", "127.0.0.1", false, key))) == 200,
          "loopback cleartext with the token must be answered");
    CHECK(status_of(ask(key, request("/v1/models", "::1", false, key))) == 200,
          "IPv6 loopback cleartext must be answered");
    CHECK(status_of(ask(key, request("/v1/models", "127.0.0.1", false, ""))) == 401,
          "loopback cleartext without the token must still be a 401");

    // An open loop (--serve on loopback with no key) keeps answering.
    CHECK(status_of(ask("", request("/v1/models", "127.0.0.1", false, ""))) == 200,
          "a loopback listener with no key must answer");
    // No key is not a licence to carry anything in the clear from off-loopback either.
    CHECK(status_of(ask("", request("/v1/models", "198.51.100.7", false, ""))) == 403,
          "an unauthenticated cleartext request from off-loopback must be refused");
    // --gateway builds ChatApi with an empty key and answers TLS: a paired device is never refused.
    CHECK(status_of(ask("", request("/v1/models", "198.51.100.7", true, ""))) == 200,
          "TLS from a gateway device must be answered with no api key");

    // /health is a probe: it answers from anywhere, over any transport.
    CHECK(status_of(ask(key, request("/health", "198.51.100.7", false, ""))) == 200,
          "/health must answer over cleartext from off-loopback");
    CHECK(status_of(ask(key, request("/health/detailed", "198.51.100.7", false, key))) == 403,
          "/health/detailed names the model and home, so it is not a probe");

    std::cout << (failures ? "serve transport policy: FAIL\n" : "serve transport policy: ok\n");
    return failures ? 1 : 0;
}
