// dd serve concurrency contract: one turn at a time, the second request is a
// 409 (never a queued duplicate, never a lock that waits), and the claim is
// released when the turn ends so the next request is accepted.
//
// It runs against the real listener and real sockets, because the bug this
// guards is invisible to a direct api.handle() call: two connections racing
// through the claim, and the busy flag left set by a turn that finished.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "dd/net/server.hpp"
#include "dd/serve/chat_api.hpp"

namespace {

int g_fail = 0;
void chk(bool ok, const std::string& what) {
    if (!ok) { ++g_fail; std::printf("FAIL %s\n", what.c_str()); }
    else std::printf("ok   %s\n", what.c_str());
}

// A turn that takes measurable time, so two requests genuinely overlap.
struct SlowRunner : dd::serve::TurnRunner {
    std::atomic<int> started{0}, finished{0};
    std::atomic<int> claims{0};
    std::string run(const std::string& msg) override {
        ++started;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        ++finished;
        return "answered:" + msg;
    }
    void interrupt() override {}
    void claimed(const dd::net::Request&) override { ++claims; }
};

int connect_to(int port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) return -1;
    return fd;
}

// Reads whatever the peer sends until EOF or the cap.
std::string slurp(int fd, size_t cap = 64u << 10) {
    std::string out;
    char buf[4096];
    while (out.size() < cap) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    return out;
}


// A complete POST with a body whose length is computed, never hand-counted:
// a wrong Content-Length makes the server wait for bytes that never come.
std::string request_for(const std::string& body) {
    return "POST /v1/chat/completions HTTP/1.1\r\n"
           "Host: x\r\nAuthorization: Bearer tok\r\nContent-Type: application/json\r\n"
           "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}
int send_all(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        const ssize_t n = ::send(fd, s.data() + off, s.size() - off, 0);
        if (n <= 0) return -1;
        off += static_cast<size_t>(n);
    }
    return 0;
}
const std::string kPlain = R"({"messages":[{"role":"user","content":"hi"}]})";
const std::string kStream = R"({"stream":true,"messages":[{"role":"user","content":"hi"}]})";
}  // namespace


int main() {
    SlowRunner runner;
    dd::serve::ApiInfo info;
    info.model_id = "ariadne";
    info.upstream_model = "local-model";
    info.home = "/srv/ariadne";
    info.tools = 4;
    dd::serve::ChatApi api(info, "tok", runner);

    dd::net::Server server("127.0.0.1", 0, [&](const dd::net::Request& q, dd::net::Conn& c) { api.handle(q, c); });
    std::string err;
    chk(server.listen(err), "listen: " + err);
    const int port = server.port();
    std::atomic<int> stop{0};
    std::thread loop([&] { server.serve(stop); });

    // Two requests that overlap: exactly one turn runs, the other is refused.
    std::atomic<int> ready{0};
    auto fire = [&](std::string& out) {
        ++ready;
        while (ready.load() < 2) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const int fd = connect_to(port);
        if (fd < 0) { out = "connect failed"; return; }
        if (send_all(fd, request_for(kPlain)) != 0) out = "send failed";
        out = slurp(fd);
        ::close(fd);
    };
    std::string a, b;
    std::thread t1([&] { fire(a); }), t2([&] { fire(b); });
    t1.join();
    t2.join();

    const bool a_ok = a.rfind("HTTP/1.1 200", 0) == 0;
    const bool b_ok = b.rfind("HTTP/1.1 200", 0) == 0;
    chk(a_ok ^ b_ok, "exactly one of two racing requests is answered 200");
    const std::string refused = a_ok ? b : a;
    chk(refused.rfind("HTTP/1.1 409", 0) == 0, "the loser is 409, not queued and not blocked");
    chk(refused.find("\"error\"") != std::string::npos, "the 409 carries an error body");
    chk(runner.started.load() == 1 && runner.finished.load() == 1, "exactly one turn ran");
    chk(runner.claims.load() == 1, "claimed() fires only for the request that won the claim");

    // The claim must be released: a request after the turn is answered again.
    {
        const int fd = connect_to(port);
        send_all(fd, request_for(kPlain));
        const std::string third = slurp(fd);
        ::close(fd);
        chk(third.rfind("HTTP/1.1 200", 0) == 0, "the next request after a finished turn is answered");
        chk(third.find("answered:hi") != std::string::npos, "the answer came back: " + third);
    }
    chk(api.busy() == false, "busy is false once the turn ended");

    // A stream racing a running turn is refused too -- the hooks belong to the
    // turn that claimed, so a second stream must never attach to them.
    {
        const int fd1 = connect_to(port);
        send_all(fd1, request_for(kStream));
        std::atomic<int> n{0};
        std::thread second([&] {
            const int fd2 = connect_to(port);
            send_all(fd2, request_for(kStream));
            const std::string r = slurp(fd2, 1024);
            n.store(r.rfind("HTTP/1.1 409", 0) == 0 ? 1 : 0);
            ::close(fd2);
        });
        const std::string sse = slurp(fd1, 64u << 10);
        second.join();
        ::close(fd1);
        chk(sse.find("text/event-stream") != std::string::npos, "the winner streams");
        chk(n.load() == 1, "a stream that races a running turn gets 409");
    }

    stop.store(1);
    loop.join();
    if (g_fail) std::printf("%d FAILED\n", g_fail);
    else std::printf("serve concurrency: all checks passed\n");
    return g_fail ? 1 : 0;
}
