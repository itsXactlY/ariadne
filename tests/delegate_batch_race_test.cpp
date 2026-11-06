// tests/delegate_batch_race_test.cpp — the parallel-batch segv of 2026-11-06, pinned.
//
// The parent's HttpLLM carries an on_wait closure that captures the TUI's scene by
// reference. run_one_child used to copy it into every child's HttpLLM, so a child
// waiting on its wire (a 429, a stall) called it from a worker thread and mutated
// the parent's scene concurrently — heap corruption that surfaced as a segv at the
// next std::thread spawn. The regression: a batch of three children on the
// *transport* path (the path production runs — the factory path never copies
// on_wait, which is why the old tests could not see this), each child's first
// request answered 429 so its wait would fire. The recorder the children would
// reach is a plain vector on purpose: under -fsanitize=thread, any child reaching
// it is a data race report, and the count the test asserts stays 0.
#include "dd/agent/agent.hpp"
#include "dd/agent/http_llm.hpp"
#include "dd/core/json.hpp"
#include "dd/core/message.hpp"
#include "dd/tools/delegate_tool.hpp"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using dd::agent::Agent;
using dd::agent::HttpLLM;
using dd::core::Json;
using dd::core::Message;
using dd::core::arr_push;
using dd::core::json_get;
using dd::core::json_num;
using dd::core::json_arr;
using dd::core::json_obj;
using dd::core::obj_set;
using dd::core::json_num;
using dd::core::json_obj;
using dd::core::json_parse;
using dd::core::json_str;

static int g_fail = 0, g_pass = 0;
static void chk(bool ok, const std::string& what) {
    if (ok) ++g_pass;
    else {
        ++g_fail;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

// ── A FIFO endpoint on loopback, one answer per connection ─────────────────────
struct FakeEndpoint {
    std::vector<std::string> answers;
    std::atomic<int> served{0};
    std::atomic<bool> stop{false};
    int fd = -1;
    std::thread th;

    explicit FakeEndpoint(std::vector<std::string> a) : answers(std::move(a)) {}

    void start() {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || ::listen(fd, 16) != 0) {
            std::printf("FAIL cannot bind a loopback endpoint\n");
            ++g_fail;
            return;
        }
        socklen_t len = sizeof(a);
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        th = std::thread([this] { loop(); });
    }

    uint16_t port() const { return port_; }

    void stop_server() {
        stop = true;
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
        if (th.joinable()) th.join();
    }

    ~FakeEndpoint() { stop_server(); }

private:
    uint16_t port_ = 0;

    static bool write_all(int c, const char* p, size_t n) {
        while (n > 0) {
            const ssize_t w = ::write(c, p, n);
            if (w <= 0) return false;
            p += static_cast<size_t>(w);
            n -= static_cast<size_t>(w);
        }
        return true;
    }

    void loop() {
        while (!stop) {
            const int c = ::accept(fd, nullptr, nullptr);
            if (c < 0) return;
            std::string req;   // read to end of headers; the body is not interesting here
            char buf[4096];
            while (req.find("\r\n\r\n") == std::string::npos) {
                const ssize_t n = ::read(c, buf, sizeof buf);
                if (n <= 0) break;
                req.append(buf, static_cast<size_t>(n));
            }
            const int i = served.fetch_add(1);
            const std::string& body = i < static_cast<int>(answers.size()) ? answers[static_cast<size_t>(i)] : kClosed;
            if (body == kClosed) {
                ::close(c);
                continue;
            }
            std::string head = body.rfind("HTTP/", 0) == 0 ? body : "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\n" + body;
            if (!write_all(c, head.c_str(), head.size())) { /* the client hung up; nothing to file */ }
            // Hold the connection a moment like a keep-alive backend, then close.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            ::close(c);
        }
    }

public:
    static const std::string kClosed;
};
const std::string FakeEndpoint::kClosed = "\x01CLOSED";

// ── The wire answers ────────────────────────────────────────────────────────────
// A 429 with a 1s Retry-After, so the child's HttpLLM waits (and, unfixed, calls
// the parent's on_wait from its worker thread), then a plain streamed completion.
static const std::string k429 =
    "HTTP/1.1 429 Too Many Requests\r\n"
    "Content-Type: application/json\r\n"
    "Retry-After: 1\r\n"
    "Content-Length: 26\r\n"
    "\r\n"
    "{\"error\":{\"code\":\"rate\"}}xx";   // 26 bytes of body; trailing xx keeps the count honest

static const std::string kDone =
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"\"},\"finish_reason\":null}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"done\"},\"finish_reason\":null}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":9}}\n\n"
    "data: [DONE]\n\n";

int main() {
    // The recorder the parent's on_wait writes into: a plain vector, no lock — if a
    // child's thread ever reaches it, TSan files the race and the count moves.
    std::vector<std::string> parent_waits;
    long parent_waits_ms = 0;

    FakeEndpoint ep({k429, k429, k429, kDone, kDone, kDone});
    ep.start();

    HttpLLM llm;
    llm.base_url = "http://127.0.0.1:" + std::to_string(ep.port());
    llm.model = "stub";
    llm.retry_attempts = 2;
    llm.on_wait = [&parent_waits, &parent_waits_ms](long status, long retry_after_s, double ms, const std::string& why) {
        parent_waits.push_back("s" + std::to_string(status) + " r" + std::to_string(retry_after_s) +
                               " " + std::to_string(ms) + " " + why);   // deliberately unsynchronized
        parent_waits_ms += static_cast<long>(ms);
    };

    Agent parent;
    parent.delegate_depth = 0;
    parent.quiet_mode = true;

    dd::tools::DelegateToolDeps deps;
    deps.parent = &parent;
    deps.transport = &llm;   // the transport path: no llm_factory, the children copy this config

    Json tasks = json_arr();
    for (int i = 0; i < 3; ++i) {
        Json t = json_obj();
        obj_set(t, "goal", json_str("say done, child " + std::to_string(i)));
        arr_push(tasks, std::move(t));
    }
    Json args = json_obj();
    obj_set(args, "tasks", std::move(tasks));

    const auto handler = dd::tools::make_delegate_tool(deps);
    const Json out = json_parse(handler(args));

    const Json* rs = dd::core::json_get(out, "results");
    chk(rs && rs->type == dd::core::Json::Arr && rs->arr.size() == 3, "three tasks, three results");
    if (rs && rs->type == dd::core::Json::Arr)
        for (size_t i = 0; i < rs->arr.size(); ++i)
            if (const dd::core::JsonSP& r = rs->arr[i]) {
                const Json* st = dd::core::json_get(*r, "status");
                const bool ok = st && st->type == dd::core::Json::Str && st->str == "completed";
                chk(ok, "child " + std::to_string(i) + " completed through the 429 (status: " +
                            (st && st->type == dd::core::Json::Str ? st->str : "?") + ")");
            }

    // The heart of the regression: the parent's on_wait was never invoked. The three
    // 429s the children saw were waited out inside the children's own HttpLLM.
    chk(parent_waits.empty(),
        "a child's wire wait never reaches the parent's on_wait (" +
            std::to_string(parent_waits.size()) + " calls seen)");

    ep.stop_server();
    std::printf("%d checks, %d failed\n", g_pass + g_fail, g_fail);
    return g_fail == 0 ? 0 : 1;
}
