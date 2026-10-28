// tests/fake_mcp_box.hpp — one loopback box that answers as a llama-server and as a
// Streamable-HTTP MCP server, shared by the tests that boot dd_tui against it.
//
// It is a fixture, not shipped code: what matters is that it records the exact bytes
// dd_tui sent, so an assertion runs on the request rather than on a guess about it.
// Two knobs exist because two proofs need them: `tools_body` is the manifest the
// server offers, and `slow_tools_ms` makes tools/list cost real time, which is the
// only way a cache that skips it can be shown to have skipped it.
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <signal.h>
#include <time.h>

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fake {

struct FakeBox {
    int fd = -1;
    uint16_t port = 0;
    std::atomic<bool> stop{false};
    std::thread th;
    mutable std::mutex mu;
    std::vector<std::string> chat_bodies;
    std::vector<std::string> mcp_log;      // every /mcp request and reply, so an RPC shape is inspectable, not guessed
    std::vector<std::string> paths_seen;   // every request line, so a missed path is nameable

    int slow_tools_ms = 0;                 // tools/list costs this much; initialize does not
    std::string tools_body = "";           // the manifest offered by tools/list, empty -> the default below

    static constexpr const char* kKey = "sk-test-ABC1234567890123456";      // 28 chars, letters+digits
    static constexpr const char* kKeyMasked = "sk-tes...3456";              // first6 + "..." + last4
    static constexpr const char* kInstrKey = "tok-server-9876543210abcd";
    static constexpr const char* kInstrMasked = "tok-se...abcd";
    // A path with a slash and digits — the shape the live servers carry in real
    // documentation, and the one ServerProse must NOT touch.
    static constexpr const char* kPath = "benchmarks/targeted_rebake/rebake.py";
    static constexpr const char* kEnum = "pulse-pro/pulse-enterprise";

    // The client picks a reply by its own id: a result without one is not an answer to
    // anything, so the RPC reads "nothing came back" and the tool never registers. Echo
    // the id the request carried.
    static std::string id_of(const std::string& body) {
        const size_t at = body.find("\"id\":");
        if (at == std::string::npos) return "1";
        size_t i = at + 5;
        std::string digits;
        while (i < body.size() && std::isdigit(static_cast<unsigned char>(body[i]))) digits.push_back(body[i++]);
        return digits.empty() ? "1" : digits;
    }

    std::string tools_json(const std::string& id) const {
        // The default manifest carries a key-shaped token and a slash-bearing path, which is
        // what the masking proof needs. A boot test replaces it with whatever it wants cached.
        std::string body = tools_body;
        if (body.empty()) {
            body = std::string("{\"name\":\"fake_tool\",") +
                   "\"description\":\"Answers a question. Key " + std::string(kKey) + " is set. " +
                   std::string(kPath) + " is the rebake harness.\"," +
                   "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"mode\":{\"type\":\"string\"," +
                   "\"enum\":[\"" + std::string(kEnum) + "\",\"plain\"]},\"q\":{\"type\":\"string\"}}," +
                   "\"required\":[\"q\"]}}";
        }
        return "{\"id\":" + id + ",\"result\":{\"tools\":[" + body + "]}}";
    }

    std::string init_json(const std::string& id) const {
        return "{\"id\":" + id + ",\"result\":{\"protocolVersion\":\"2025-03-26\",\"capabilities\":{},"
               "\"serverInfo\":{\"name\":\"fake\",\"version\":\"1\"},"
               "\"instructions\":\"Call fake_tool with " + std::string(kInstrKey) + " when unsure.\"}}";
    }

    static std::string sse() {
        return "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
               "data: {\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"\"},"
               "\"finish_reason\":null}]}\n\n"
               "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"ok\"},\"finish_reason\":null}]}\n\n"
               "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}],"
               "\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":2}}\n\n"
               "data: [DONE]\n\n";
    }

    static std::string with_len(long status, const std::string& body, const std::string& ctype) {
        std::string head = "HTTP/1.1 " + std::to_string(status) + " OK\r\n";
        head += "Content-Type: " + ctype + "\r\nContent-Length: " + std::to_string(body.size()) +
                "\r\nConnection: close\r\n\r\n";
        return head + body;
    }

    void start() {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || ::listen(fd, 8) != 0) {
            std::printf("FAIL cannot bind a loopback box\n");
            return;
        }
        socklen_t len = sizeof(a);
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
        th = std::thread([this] { run(); });
    }

    void halt() {
        stop.store(true);
        if (th.joinable()) th.join();
        if (fd >= 0) { ::close(fd); fd = -1; }
    }

    // A second boot is measured against the first, so the counters have to be per run,
    // not cumulative: without this, "the second boot listed nothing" would be false by
    // arithmetic rather than by behaviour.
    void reset() {
        std::lock_guard<std::mutex> lk(mu);
        chat_bodies.clear();
        mcp_log.clear();
        paths_seen.clear();
    }

    // How many tools/list RPCs the box answered since the last reset, and whether it
    // handshook at all. These two numbers are the whole cache proof: a served boot
    // handshakes and does not list.
    int count_tools_lists() const {
        std::lock_guard<std::mutex> lk(mu);
        int n = 0;
        for (const auto& e : mcp_log)
            if (e.find("tools/list") != std::string::npos) ++n;
        return n;
    }
    int count_initializes() const {
        std::lock_guard<std::mutex> lk(mu);
        int n = 0;
        for (const auto& e : mcp_log)
            if (e.find("initialize") != std::string::npos) ++n;
        return n;
    }

    void run() {
        for (;;) {
            pollfd p{fd, POLLIN, 0};
            if (::poll(&p, 1, 150) <= 0) {
                if (stop.load()) return;
                continue;
            }
            const int c = ::accept(fd, nullptr, nullptr);
            if (c < 0) return;

            std::string req;
            size_t need = 0;
            bool headers = false;
            while (!headers || req.size() < need) {
                char buf[2048];
                const ssize_t n = ::recv(c, buf, sizeof(buf), 0);
                if (n <= 0) break;
                req.append(buf, static_cast<size_t>(n));
                if (!headers && req.find("\r\n\r\n") != std::string::npos) {
                    headers = true;
                    const size_t body_at = req.find("\r\n\r\n") + 4;
                    const size_t at = req.find("Content-Length: ");
                    need = body_at + (at < body_at ? std::strtoul(req.c_str() + at + 16, nullptr, 10) : 0);
                }
            }
            if (!headers) { ::close(c); continue; }

            const size_t body_at = req.find("\r\n\r\n") + 4;
            // The request line is "METHOD PATH VERSION\r\n": the path is the span between
            // the two spaces. Reading it up to the first \r\n would hand back
            // "/mcp HTTP/1.1" and every branch would miss.
            const size_t sp = req.find(' ');
            const size_t sp2 = sp < req.size() ? req.find(' ', sp + 1) : std::string::npos;
            const std::string path = sp2 != std::string::npos ? req.substr(sp + 1, sp2 - sp - 1) : std::string();
            {
                std::lock_guard<std::mutex> lk(mu);
                paths_seen.push_back(path);
            }
            const std::string body = req.substr(body_at);

            std::string resp;
            if (path == "/props") {
                resp = with_len(200, "{\"n_ctx\":4096,\"build_info\":\"llama-b9000\",\"total_slots\":2}",
                                "application/json");
            } else if (path == "/slots") {
                resp = with_len(200, "[{\"id\":0,\"n_prompt_tokens\":0,\"is_processing\":false}]", "application/json");
            } else if (path.rfind("/mcp", 0) == 0) {   // /mcp and any path a re-keyed config points it at
                // The RPC is dispatched by method; a notification has no id and gets 202.
                const bool notif = body.find("notifications/") != std::string::npos;
                if (notif) {
                    resp = "HTTP/1.1 202 Accepted\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                } else {
                    // A reply is only a reply if it carries the id the request sent.
                    const std::string id = id_of(body);
                    if (body.find("initialize") != std::string::npos) {
                        resp = with_len(200, init_json(id), "application/json");
                    } else if (body.find("tools/list") != std::string::npos) {
                        if (slow_tools_ms > 0) ::usleep(static_cast<useconds_t>(slow_tools_ms) * 1000);
                        resp = with_len(200, tools_json(id), "application/json");
                    } else {
                        resp = with_len(200, "{\"id\":" + id + ",\"result\":{}}", "application/json");
                    }
                }
            } else if (path == "/v1/chat/completions") {
                {
                    std::lock_guard<std::mutex> lk(mu);
                    chat_bodies.push_back(body);
                }
                resp = sse();
            } else {
                resp = with_len(404, "{\"error\":\"unknown path\"}", "application/json");
            }
            if (path.rfind("/mcp", 0) == 0) { std::lock_guard<std::mutex> lk(mu); mcp_log.push_back(body + "\n---\n" + resp); }
            ::send(c, resp.data(), resp.size(), MSG_NOSIGNAL);
            ::close(c);
        }
    }
};

// The whole file, for a test that asserts on bytes rather than on a parse.
inline std::string slurp(const std::string& path) {
    int f = ::open(path.c_str(), O_RDONLY);
    if (f < 0) return {};
    std::string out;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(f, buf, sizeof(buf));
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    ::close(f);
    return out;
}

// Run dd_tui to completion against this box and say how long it took. Wall time is the
// measurement the cache proof turns on: the box costs tools/list real milliseconds, so
// a boot that skipped it is visible in the clock as well as in the request log.
inline long run_dd_tui(const std::string& home, const std::string& base, const std::string& out_path, int* status) {
    const pid_t pid = ::fork();
    if (pid == 0) {
        const int out = ::open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (out >= 0) { ::dup2(out, STDOUT_FILENO); ::dup2(out, STDERR_FILENO); ::close(out); }
        const int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull >= 0) { ::dup2(devnull, STDIN_FILENO); ::close(devnull); }
        setenv("DD_BASE_URL", base.c_str(), 1);
        setenv("DD_NO_SOAK", "1", 1);       // nothing is written to the maze; the cache file is not the maze
        setenv("DD_MAX_TURNS", "1", 1);
        const std::string homearg = "--home";
        const std::vector<const char*> argv = {"build/ariadne", homearg.c_str(), home.c_str(), "--", "hi", nullptr};
        ::execv(argv[0], const_cast<char* const*>(argv.data()));
        ::_exit(127);
    }

    timespec t0 {};
    ::clock_gettime(CLOCK_MONOTONIC, &t0);
    int s = -1;
    // Reap at 10ms resolution. Polling once a second would report every boot as exactly
    // 1000ms and make a timing claim about a cache that saves hundreds of milliseconds a
    // claim about the test's own granularity.
    for (int waited = 0; waited < 9000; ++waited) {
        int st = 0;
        if (::waitpid(pid, &st, WNOHANG) > 0) { s = st; break; }
        ::usleep(10000);
    }
    if (s < 0) { ::kill(pid, SIGKILL); ::waitpid(pid, &s, 0); }
    timespec t1 {};
    ::clock_gettime(CLOCK_MONOTONIC, &t1);
    *status = s;
    return (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
}

}  // namespace fake
