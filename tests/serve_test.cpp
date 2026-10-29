// Verifies dd::net::Server + dd::serve::ChatApi: request parsing, the
// OpenAI-compatible routes, SSE chunks of one turn, the one-turn-at-a-time
// claim, bearer auth, and that a client leaving mid-stream interrupts the turn.
#include "dd/net/server.hpp"
#include "dd/serve/chat_api.hpp"
#include "dd/core/json.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

using namespace dd;

static int failures = 0;
#define CHECK(cond, msg)                                   \
    do {                                                   \
        if (!(cond)) {                                     \
            std::cerr << "FAIL: " << msg << "\n";          \
            ++failures;                                    \
        }                                                  \
    } while (0)

// A turn that talks through the same hooks the harness wires.
struct FakeRunner : serve::TurnRunner {
    serve::ChatApi* api = nullptr;
    std::atomic<bool> interrupted{false};
    std::atomic<bool> hold{false};      // keep the turn running until released
    std::atomic<bool> started{false};
    std::string last_;               // published by `started` (release), read after acquiring it
    std::string last() const { return started.load(std::memory_order_acquire) ? last_ : ""; }
    std::string run(const std::string& msg) override {
        last_ = msg;
        started.store(true, std::memory_order_release);
        api->on_reasoning("thinking");
        api->on_tool_call("call_1", "read_file", R"({"path":"README.md"})");
        api->on_tool_result("call_1", std::string(serve::ChatApi::kToolResultMax + 10, 'x'));
        api->on_content("po");
        api->on_content("ng");
        // a held turn keeps producing, as a real one does — on its own thread, where the hooks fire
        while (hold && !interrupted) {
            api->on_content(".");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return "pong";
    }
    void interrupt() override { interrupted = true; }
};

static std::string roundtrip(int port, const std::string& raw, bool read_all = true) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { ::close(fd); return ""; }
    ::send(fd, raw.data(), raw.size(), MSG_NOSIGNAL);
    std::string out;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
        if (!read_all) break;
    }
    ::close(fd);
    return out;
}

static std::string post(const std::string& path, const std::string& body, const std::string& auth = "") {
    std::string r = "POST " + path + " HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n";
    if (!auth.empty()) r += "Authorization: Bearer " + auth + "\r\n";
    return r + "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

static std::string get(const std::string& path, const std::string& auth = "") {
    std::string r = "GET " + path + " HTTP/1.1\r\nHost: x\r\n";
    if (!auth.empty()) r += "Authorization: Bearer " + auth + "\r\n";
    return r + "\r\n";
}

static void parse_cases() {
    net::Request r;
    CHECK(net::parse_request("GET /x HTTP/1.1\r\nHost: a\r\n", r, 100) == net::Parse::Incomplete, "headers not ended");
    CHECK(net::parse_request("GET /v1/models?x=1 HTTP/1.1\r\nHOST: a\r\n\r\n", r, 100) == net::Parse::Done, "plain GET");
    CHECK(r.path == "/v1/models" && r.query == "x=1" && r.header("host") == "a", "path, query, lowercased header");
    CHECK(net::parse_request("POST /c HTTP/1.1\r\nContent-Length: 5\r\n\r\nab", r, 100) == net::Parse::Incomplete,
          "body still arriving");
    CHECK(net::parse_request("POST /c HTTP/1.1\r\nContent-Length: 2\r\n\r\nab", r, 100) == net::Parse::Done && r.body == "ab",
          "body read");
    CHECK(net::parse_request("POST /c HTTP/1.1\r\nContent-Length: 500\r\n\r\n", r, 100) == net::Parse::TooLarge, "body cap");
    CHECK(net::parse_request("BROKEN\r\n\r\n", r, 100) == net::Parse::Bad, "no request line");
    CHECK(net::parse_request("GET x HTTP/1.1\r\n\r\n", r, 100) == net::Parse::Bad, "target without a slash");
    CHECK(net::parse_request("POST /c HTTP/1.1\r\nContent-Length: -1\r\n\r\n", r, 100) == net::Parse::Bad, "bad length");
    CHECK(net::parse_request("POST /c HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", r, 100) == net::Parse::Bad,
          "chunked body refused");
}

int main() {
    parse_cases();

    FakeRunner runner;
    serve::ApiInfo info;
    info.upstream_model = "local-model";
    info.home = "/srv/ariadne";
    info.tools = 7;
    serve::ChatApi api(info, "k3y", runner);
    runner.api = &api;
    net::Server server("127.0.0.1", 0, [&](const net::Request& q, net::Conn& c) { api.handle(q, c); });
    std::string err;
    CHECK(server.listen(err), "listen: " + err);
    const int port = server.port();
    std::atomic<int> stop{0};
    std::thread loop([&] { server.serve(stop); });

    // open health, guarded everything else
    CHECK(roundtrip(port, get("/health")).find("\"agent\":\"ariadne\"") != std::string::npos, "health is open");
    CHECK(roundtrip(port, get("/v1/models")).rfind("HTTP/1.1 401", 0) == 0, "models without a token is 401");
    const std::string models = roundtrip(port, get("/v1/models", "k3y"));
    CHECK(models.find("\"id\":\"ariadne\"") != std::string::npos, "models lists ariadne");
    const std::string detail = roundtrip(port, get("/health/detailed", "k3y"));
    CHECK(detail.find("\"tools\":7") != std::string::npos && detail.find("\"busy\":false") != std::string::npos,
          "detailed health: " + detail);
    CHECK(roundtrip(port, get("/nope", "k3y")).rfind("HTTP/1.1 404", 0) == 0, "unknown route 404");
    CHECK(roundtrip(port, post("/v1/chat/completions", "{x", "k3y")).rfind("HTTP/1.1 400", 0) == 0, "bad JSON 400");
    CHECK(roundtrip(port, post("/v1/chat/completions", R"({"messages":[]})", "k3y")).rfind("HTTP/1.1 400", 0) == 0,
          "no user message 400");

    // non-streaming: the last user message is the turn, the answer comes back whole
    const std::string body = R"({"model":"ariadne","messages":[{"role":"user","content":"old"},)"
                             R"({"role":"assistant","content":"x"},{"role":"user","content":[{"type":"text","text":"ping"}]}]})";
    const std::string plain = roundtrip(port, post("/v1/chat/completions", body, "k3y"));
    CHECK(plain.rfind("HTTP/1.1 200", 0) == 0, "chat 200");
    CHECK(runner.last() == "ping", "the last user message is the turn: " + runner.last());
    CHECK(plain.find("\"content\":\"pong\"") != std::string::npos && plain.find("chat.completion") != std::string::npos,
          "non-stream answer: " + plain);
    CHECK(plain.find("ariadne_tool") == std::string::npos, "tool events only ride a stream");

    // streaming: role, reasoning, tool call, cut tool result, content, finish, [DONE]
    const std::string sbody = R"({"stream":true,"messages":[{"role":"user","content":"ping"}]})";
    const std::string sse = roundtrip(port, post("/v1/chat/completions", sbody, "k3y"));
    CHECK(sse.find("text/event-stream") != std::string::npos, "SSE content type");
    const size_t role = sse.find(R"("role":"assistant")");
    const size_t reason = sse.find(R"("reasoning_content":"thinking")");
    const size_t call = sse.find(R"("ariadne_tool_call":{"id":"call_1","name":"read_file")");
    const size_t result = sse.find(R"("truncated":true)");
    const size_t content = sse.find(R"("content":"po")");
    const size_t finish = sse.find(R"("finish_reason":"stop")");
    const size_t done = sse.find("data: [DONE]");
    CHECK(role != std::string::npos && reason != std::string::npos && call != std::string::npos &&
              result != std::string::npos && content != std::string::npos && finish != std::string::npos &&
              done != std::string::npos,
          "every event present: " + sse.substr(0, 600));
    CHECK(role < reason && reason < call && call < result && result < content && content < finish && finish < done,
          "events in order");

    // one turn at a time: a second chat while one runs is 409, health still answers
    runner.hold = true;
    runner.started = false;
    runner.interrupted = false;
    std::thread first([&] { roundtrip(port, post("/v1/chat/completions", sbody, "k3y")); });
    while (!runner.started) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK(roundtrip(port, post("/v1/chat/completions", sbody, "k3y")).rfind("HTTP/1.1 409", 0) == 0,
          "second turn refused while one runs");
    CHECK(roundtrip(port, get("/health/detailed", "k3y")).find("\"busy\":true") != std::string::npos, "busy reported");
    runner.hold = false;
    first.join();
    CHECK(!api.busy(), "the claim is released after the turn");

    // a client that leaves mid-stream interrupts the turn
    runner.hold = true;
    runner.started = false;
    runner.interrupted = false;
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<uint16_t>(port));
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
        const std::string raw = post("/v1/chat/completions", sbody, "k3y");
        ::send(fd, raw.data(), raw.size(), MSG_NOSIGNAL);
        while (!runner.started) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        ::close(fd);
    }
    // the held turn keeps writing; the first write to the closed socket fails and interrupts it
    for (int i = 0; i < 400 && !runner.interrupted; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(runner.interrupted.load(), "a client that left interrupts the turn");
    runner.hold = false;
    while (api.busy()) std::this_thread::sleep_for(std::chrono::milliseconds(2));

    stop = 1;
    loop.join();
    if (failures == 0) std::cout << "serve: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
