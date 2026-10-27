// tests/mcp_prose_mask_test.cpp — W5: a server's own prose is masked where it
// enters the prompt, and its schema is not.
//
// The proof is end-to-end, not a unit on the masker: a fake MCP server hands a
// description and instructions that carry a key-shaped token, a fake endpoint
// records the exact bytes dd_tui sends, and the assertions run on those bytes.
// Nothing is asserted about what the masker "should" do — the request body is the
// thing that ships, so the request body is what is checked. No tokens are spent:
// the fake endpoint answers with a canned stream.
#include "dd/agent/redact.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_fail = 0;
void chk(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

// One loopback box answering four paths: /props and /slots so the endpoint probe
// accepts it as a llama-server, /mcp for the Streamable-HTTP RPC, and
// /v1/chat/completions, whose body is what we are actually testing.
struct FakeBox {
    int fd = -1;
    uint16_t port = 0;
    std::atomic<bool> stop{false};
    std::thread th;
    std::mutex mu;
    std::vector<std::string> chat_bodies;
    std::vector<std::string> mcp_log;   // every /mcp request and reply, so an RPC shape is inspectable, not guessed
    std::vector<std::string> paths_seen;    // every request line, so a missed path is nameable

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
        return "{\"id\":" + id + ",\"result\":{\"tools\":[{\"name\":\"fake_tool\","
               "\"description\":\"Answers a question. Key " + std::string(kKey) + " is set. " +
               std::string(kPath) + " is the rebake harness.\","
               "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"mode\":{\"type\":\"string\","
               "\"enum\":[\"" + std::string(kEnum) + "\",\"plain\"]},\"q\":{\"type\":\"string\"}},"
               "\"required\":[\"q\"]}}]}}";
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
            ++g_fail;
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
            } else if (path == "/mcp") {
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
            if (path == "/mcp") { std::lock_guard<std::mutex> lk(mu); mcp_log.push_back(body + "\n---\n" + resp); }
            ::send(c, resp.data(), resp.size(), MSG_NOSIGNAL);
            ::close(c);
        }
    }
};

std::string read_file(const std::string& path) {
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

// The masking rule, asserted directly: the two policies differ exactly where a
// slash-bearing token sits, which is the difference between a credential and a path.
void probe_rule() {
    using namespace dd::agent;
    const std::string key = FakeBox::kKey;
    chk(mask_secret_tokens("Answers a question. Key " + key + " is set.") ==
            "Answers a question. Key " + std::string(FakeBox::kKeyMasked) + " is set.",
        "a key-shaped token is masked");
    chk(mask_secret_tokens(FakeBox::kPath, SecretPolicy::ServerProse) == FakeBox::kPath,
        "a slash path in server prose stays byte-identical");
    chk(mask_secret_tokens(FakeBox::kEnum, SecretPolicy::ServerProse) == FakeBox::kEnum,
        "an enum value in server prose stays byte-identical");
    chk(mask_secret_tokens(FakeBox::kPath, SecretPolicy::ToolOutput) != FakeBox::kPath,
        "the same path in tool output is masked (the shipped rule)");
    chk(mask_secret_tokens("short 12345678 token and /plain/path", SecretPolicy::ToolOutput) ==
            "short 12345678 token and /plain/path",
        "short prose and short paths stay");
    // Whitespace survives byte for byte, so masking cannot reflow a description.
    const std::string nl = "line one\n" + key + "\nline three\n";
    const std::string m = mask_secret_tokens(nl, SecretPolicy::ServerProse);
    chk(m == "line one\n" + std::string(FakeBox::kKeyMasked) + "\nline three\n", "newline placement survives");
}

}  // namespace

int main() {
    probe_rule();
    if (g_fail) return 1;

    // ── end-to-end: what dd_tui actually sends ──
    FakeBox box;
    box.start();
    if (box.port == 0) return 1;

    const std::string home = "/tmp/dd_prose_mask_home";
    ::system("rm -rf /tmp/dd_prose_mask_home && mkdir -p /tmp/dd_prose_mask_home");
    const std::string base = "http://127.0.0.1:" + std::to_string(box.port) + "/v1";
    // tool_search off: the deck defers anything past its threshold behind the search bridges,
    // and a deferred tool never rides the prompt, so its description could not be observed there.
    const std::string cfg = "model:\n  base_url: " + base + "\n  name: fake-model\n"
                            "tool_search:\n  enabled: false\n"
                            "mcp_servers:\n  fake:\n    enabled: true\n    transport: http\n"
                            "    url: http://127.0.0.1:" + std::to_string(box.port) + "/mcp\n"
                            "    instructions: verbatim\n";
    {
        int f = ::open((home + "/config.yaml").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (f < 0) { std::printf("FAIL cannot write the temp home\n"); ++g_fail; box.halt(); return 1; }
        ::write(f, cfg.data(), cfg.size());
        ::close(f);
    }

    const std::string out_path = "/tmp/dd_prose_mask.out";
    const pid_t pid = ::fork();
    if (pid == 0) {
        const int out = ::open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (out >= 0) { ::dup2(out, STDOUT_FILENO); ::dup2(out, STDERR_FILENO); ::close(out); }
        const int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull >= 0) { ::dup2(devnull, STDIN_FILENO); ::close(devnull); }
        setenv("DD_BASE_URL", base.c_str(), 1);
        setenv("DD_NO_SOAK", "1", 1);
        setenv("DD_MAX_TURNS", "1", 1);
        std::string homearg = "--home";
        std::vector<const char*> argv = {"build/dd_tui", homearg.c_str(), home.c_str(), "--", "hi", nullptr};
        ::execv(argv[0], const_cast<char* const*>(argv.data()));
        ::_exit(127);
    }

    int status = -1;
    for (int waited = 0; waited < 90; ++waited) {
        int s = 0;
        if (::waitpid(pid, &s, WNOHANG) > 0) { status = s; break; }
        ::sleep(1);
    }
    if (status < 0) { ::kill(pid, SIGKILL); ::waitpid(pid, &status, 0); }

    box.halt();

    std::string body;
    {
        std::lock_guard<std::mutex> lk(box.mu);
        if (box.chat_bodies.empty()) {
            std::printf("FAIL dd_tui never reached the endpoint (exit %d)\n", status);
            std::printf("paths the box saw:");
            for (const auto& s : box.paths_seen) std::printf(" %s", s.c_str());
            std::printf("\n");
            std::printf("--- dd_tui output ---\n%s\n", read_file(out_path).substr(0, 2000).c_str());
            ++g_fail;
            return 1;
        }
        body = box.chat_bodies.front();
        {   // the body is the datum; write it so a failure is inspectable, not guessed at
            int f = ::open("/tmp/dd_prose_mask_body.json", O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (f >= 0) { ::write(f, body.data(), body.size()); ::close(f); }
            std::printf("chat bodies captured: %zu\n", box.chat_bodies.size());
        }
    }

    // The wire list is the datum; these two dumps make a failure nameable rather than guessed at.
    // The wire list is the datum; these two dumps make a failure nameable rather than guessed at.
    {
        int f = ::open("/tmp/dd_prose_mask_mcp.log", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (f >= 0) { for (const auto& s : box.mcp_log) ::write(f, s.data(), s.size()); ::close(f); }
    }
    chk(body.find(FakeBox::kKey) == std::string::npos, "the server's key never rides the prompt");
    chk(body.find(FakeBox::kKeyMasked) != std::string::npos, "the masked description rides the prompt");
    chk(body.find(FakeBox::kInstrKey) == std::string::npos, "the server's instructions key never rides");
    chk(body.find(FakeBox::kInstrMasked) != std::string::npos, "the masked instructions ride the system prompt");
    chk(body.find("\"fake_tool\"") != std::string::npos, "the tool name stays byte-identical");
    chk(body.find(FakeBox::kEnum) != std::string::npos, "the enum value stays byte-identical");
    chk(body.find(FakeBox::kPath) != std::string::npos, "the path in the description stays byte-identical");
    chk(body.find("\"required\":[\"q\"]") != std::string::npos, "the schema node is deep-copied untouched");

    if (g_fail == 0) std::printf("server-prose masking: all ok\n");
    return g_fail ? 1 : 0;
}
