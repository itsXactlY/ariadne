// Server::serve must return when stop is set, including while handlers are
// mid-flight — and it must return within a bounded grace, never forever.
//
// Two shapes are tested, because the old defect was invisible in both:
//   * stop during a handler that then finishes normally -> serve returns, the
//     handler's own write is allowed to land.
//   * stop during a handler that never finishes -> serve returns anyway once
//     the grace expires. Without a bound the process hangs on exit forever.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "dd/net/server.hpp"

namespace {

int g_fail = 0;
void chk(bool ok, const std::string& what) {
    if (!ok) { ++g_fail; std::printf("FAIL %s\n", what.c_str()); }
    else std::printf("ok   %s\n", what.c_str());
}

int connect_to(int port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) return -1;
    return fd;
}

void send_all(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        const ssize_t n = ::send(fd, s.data() + off, s.size() - off, 0);
        if (n <= 0) return;
        off += static_cast<size_t>(n);
    }
}

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

const std::string kGet = "GET /x HTTP/1.1\r\nHost: x\r\n\r\n";

}  // namespace

int main() {
    // --- stop lands while a handler is mid-flight ---
    std::atomic<int> finished{0};
    dd::net::Server slow("127.0.0.1", 0, [&](const dd::net::Request&, dd::net::Conn& c) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        c.respond(200, "text/plain", "late");
        ++finished;
    });
    std::string err;
    chk(slow.listen(err), "listen: " + err);
    const int port = slow.port();
    std::atomic<int> stop{0};
    std::thread loop([&] { slow.serve(stop); });

    const int fd = connect_to(port);
    chk(fd >= 0, "client connected");
    send_all(fd, kGet);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));   // the handler is sleeping
    stop.store(1);
    const auto t0 = std::chrono::steady_clock::now();
    loop.join();
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    const std::string resp = slurp(fd);
    ::close(fd);
    chk(waited >= 0 && waited < 5000, "serve() returned with a handler in flight, bounded: " + std::to_string(waited) + "ms");
    chk(finished.load() == 1, "the in-flight handler was allowed to finish its write");
    chk(resp.find("HTTP/1.1 200") != std::string::npos, "the response landed");

    // --- a handler that never returns must not pin serve() forever ---
    std::atomic<bool> release{false};
    dd::net::Server stuck("127.0.0.1", 0, [&](const dd::net::Request&, dd::net::Conn&) {
        while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    });
    chk(stuck.listen(err), "listen: " + err);
    std::atomic<int> stop2{0};
    std::thread loop2([&] { stuck.serve(stop2); });
    const int fd2 = connect_to(stuck.port());
    send_all(fd2, kGet);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));   // handler is stuck
    stop2.store(1);
    const auto t1 = std::chrono::steady_clock::now();
    loop2.join();
    const auto waited2 = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t1).count();
    release.store(true);
    ::close(fd2);
    chk(waited2 <= 6000, "serve() returned despite a handler that never finishes: " + std::to_string(waited2) + "ms");

    if (g_fail) std::printf("%d FAILED\n", g_fail);
    else std::printf("server shutdown: all checks passed\n");
    return g_fail ? 1 : 0;
}
