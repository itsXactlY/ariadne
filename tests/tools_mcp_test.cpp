// MCP client, the one-shot HTTP helper and the LLM transport against a throwaway server on 127.0.0.1.
// The server answers every reply shape a Streamable-HTTP MCP server may use and records what it was sent.
#include <arpa/inet.h>
#include <curl/curl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "dd/agent/http_llm.hpp"
#include "dd/core/json.hpp"
#include "dd/net/http.hpp"
#include "dd/tools/mcp.hpp"

using dd::core::Json;
using dd::tools::MCPClient;

static int g_fail = 0;
static void chk(bool ok, const std::string& label) {
    if (!ok) { ++g_fail; std::printf("FAIL %s\n", label.c_str()); }
}
static bool has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

struct Req {
    std::string line, body;
    std::vector<std::pair<std::string, std::string>> headers;   // names lower-cased
    std::string header(const std::string& n) const {
        for (const auto& [k, v] : headers)
            if (k == n) return v;
        return "";
    }
    bool has_header(const std::string& n) const {
        for (const auto& [k, v] : headers)
            if (k == n) return true;
        return false;
    }
};

enum Mode {
    kSse, kBare, kPretty, kBatch, kRpcError, kIsError, kWrongId, kHttp500, kBig, kMultiline, kCrlf, kInit, kPages,
    kEmpty, kStructured, kResource, kLlmStream
};

class Server {
public:
    std::atomic<int> mode{kSse};
    std::atomic<int> served{0};   // seen[0 .. served) are complete

    bool start() {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(fd_, 16) != 0) return false;
        socklen_t len = sizeof a;
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        seen_.reserve(1024);
        th_ = std::thread([this] { run(); });
        return true;
    }
    ~Server() {
        stop_.store(true, std::memory_order_release);
        if (th_.joinable()) th_.join();
        if (fd_ >= 0) ::close(fd_);
    }
    std::string base() const { return "http://127.0.0.1:" + std::to_string(port_); }
    std::string url(const std::string& path) const { return base() + path; }
    const Req& req(int i) const { return seen_[static_cast<size_t>(i)]; }
    void wait_served(int n) const {
        for (int spin = 0; served.load(std::memory_order_acquire) < n && spin < 2000; ++spin)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

private:
    int fd_ = -1, port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread th_;
    std::vector<Req> seen_;

    void run() {
        while (!stop_.load(std::memory_order_acquire)) {
            pollfd p{fd_, POLLIN, 0};
            if (::poll(&p, 1, 50) <= 0) continue;
            const int c = ::accept(fd_, nullptr, nullptr);
            if (c < 0) continue;
            timeval tv{2, 0};
            ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            handle(c);
            ::close(c);
        }
    }

    static void send_all(int c, const std::string& s) {
        size_t off = 0;
        while (off < s.size()) {
            const ssize_t w = ::send(c, s.data() + off, s.size() - off, MSG_NOSIGNAL);
            if (w <= 0) return;
            off += static_cast<size_t>(w);
        }
    }

    static std::string http(int status, const std::string& ctype, const std::string& body, const std::string& extra = "") {
        const char* text = status == 200 ? "OK" : status == 202 ? "Accepted" : status == 500 ? "Internal Server Error" : "Status";
        return "HTTP/1.1 " + std::to_string(status) + " " + text + "\r\nContent-Type: " + ctype +
               "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n" + extra + "\r\n" + body;
    }

    static std::string tool_result(long long id, const std::string& text, bool is_error = false) {
        return "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) + ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"" + text +
               "\"}]" + (is_error ? ",\"isError\":true" : "") + "}}";
    }

    void handle(int c) {
        std::string buf;
        char tmp[4096];
        size_t end;
        while ((end = buf.find("\r\n\r\n")) == std::string::npos) {
            const ssize_t r = ::recv(c, tmp, sizeof tmp, 0);
            if (r <= 0) return;
            buf.append(tmp, static_cast<size_t>(r));
        }
        Req rq;
        const std::string head = buf.substr(0, end);
        rq.body = buf.substr(end + 4);
        size_t pos = head.find("\r\n");
        rq.line = head.substr(0, pos);
        long clen = 0;
        while (pos != std::string::npos) {
            const size_t next = head.find("\r\n", pos + 2);
            const std::string line = head.substr(pos + 2, next == std::string::npos ? std::string::npos : next - pos - 2);
            pos = next;
            const size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string k = line.substr(0, colon), v = line.substr(colon + 1);
            for (auto& ch : k) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            while (!v.empty() && v.front() == ' ') v.erase(v.begin());
            if (k == "content-length") clen = std::atol(v.c_str());
            rq.headers.emplace_back(k, v);
        }
        while (static_cast<long>(rq.body.size()) < clen) {
            const ssize_t r = ::recv(c, tmp, sizeof tmp, 0);
            if (r <= 0) break;
            rq.body.append(tmp, static_cast<size_t>(r));
        }
        seen_.push_back(rq);
        served.fetch_add(1, std::memory_order_release);
        send_all(c, reply(rq));
    }

    std::string reply(const Req& rq) {
        const std::string path = rq.line.substr(rq.line.find(' ') + 1, rq.line.rfind(' ') - rq.line.find(' ') - 1);
        if (path == "/small") return http(200, "text/plain", "ok");
        if (path == "/big") return http(200, "text/plain", std::string(64 * 1024, 'x'));
        const int m = mode.load(std::memory_order_acquire);
        if (has(path, "chat/completions")) {
            return "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
                   "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n"
                   "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":3,\"completion_tokens\":1}}\n\n"
                   "data: [DONE]\n\n";
        }
        long long id = 0;
        std::string method, cursor;
        try {
            const Json j = dd::core::json_parse(rq.body);
            if (const Json* v = dd::core::json_get(j, "id"); v && v->type == Json::Num) id = static_cast<long long>(v->num);
            if (const Json* v = dd::core::json_get(j, "method"); v && v->type == Json::Str) method = v->str;
            if (const Json* p = dd::core::json_get(j, "params"))
                if (const Json* cu = dd::core::json_get(*p, "cursor"); cu && cu->type == Json::Str) cursor = cu->str;
        } catch (...) {
        }
        if (method.rfind("notifications/", 0) == 0) return http(202, "text/plain", "");
        const std::string sse = "text/event-stream", js = "application/json";
        const std::string n = std::to_string(id);
        switch (m) {
        case kSse: return http(200, sse, "event: message\ndata: " + tool_result(id, "hello") + "\n\n");
        case kBare: return http(200, js, tool_result(id, "hello"));
        case kPretty:
            return http(200, js, "{\n  \"jsonrpc\": \"2.0\",\n  \"id\": " + n + ",\n  \"result\": {\n    \"content\": [{\"type\": \"text\", \"text\": \"hello\"}]\n  }\n}\n");
        case kBatch: return http(200, js, "[" + tool_result(id + 50, "wrong") + "," + tool_result(id, "hello") + "]");
        case kRpcError:
            return http(200, sse, "data: {\"jsonrpc\":\"2.0\",\"id\":" + n + ",\"error\":{\"code\":-32601,\"message\":\"nope\"}}\n\n");
        case kIsError: return http(200, js, tool_result(id, "boom", true));
        case kWrongId:
            return http(200, sse,
                        "data: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/message\",\"params\":{\"data\":\"log\"}}\n\n"
                        "data: " + tool_result(id + 100, "wrong") + "\n\n"
                        "data: " + tool_result(id, "right") + "\n\n");
        case kHttp500: return http(500, "text/plain", "upstream exploded\nbadly");
        case kBig: return http(200, sse, "data: " + std::string(64 * 1024, 'x') + "\n\n");
        case kMultiline: {
            const std::string r = tool_result(id, "joined");
            const size_t cut = r.find("\"result\"");
            return http(200, sse, "data: " + r.substr(0, cut) + "\ndata: " + r.substr(cut) + "\n\n");
        }
        case kCrlf: return http(200, sse, "event: message\r\ndata: " + tool_result(id, "crlf") + "\r\n\r\n");
        case kInit:
            if (method == "initialize")
                return http(200, js,
                            "{\"jsonrpc\":\"2.0\",\"id\":" + n + ",\"result\":{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
                            "\"serverInfo\":{\"name\":\"t\",\"version\":\"1\"},\"instructions\":\"be nice\"}}",
                            "Mcp-Session-Id: sess-123\r\n");
            if (method == "tools/list") return http(200, js, "{\"jsonrpc\":\"2.0\",\"id\":" + n + ",\"result\":{\"tools\":[{\"name\":\"echo\"}]}}");
            return http(200, js, tool_result(id, "ok-session"));
        case kPages:
            if (cursor == "p2") return http(200, js, "{\"jsonrpc\":\"2.0\",\"id\":" + n + ",\"result\":{\"tools\":[{\"name\":\"b\"}]}}");
            return http(200, js, "{\"jsonrpc\":\"2.0\",\"id\":" + n + ",\"result\":{\"tools\":[{\"name\":\"a\"}],\"nextCursor\":\"p2\"}}");
        case kEmpty: return http(200, js, "");
        case kStructured:
            return http(200, js, "{\"jsonrpc\":\"2.0\",\"id\":" + n + ",\"result\":{\"content\":[],\"structuredContent\":{\"n\":3}}}");
        case kResource:
            return http(200, js,
                        "{\"jsonrpc\":\"2.0\",\"id\":" + n + ",\"result\":{\"content\":[{\"type\":\"image\",\"data\":\"AA==\"},"
                        "{\"type\":\"resource\",\"resource\":{\"uri\":\"x\",\"text\":\"from a resource\"}}]}}");
        default: return http(200, js, tool_result(id, "default"));
        }
    }
};

// accepts, sends the response head, then says nothing
static void run_stall_server(int listen_fd, std::atomic<bool>& stop, std::atomic<int>& accepted) {
    while (!stop.load(std::memory_order_acquire)) {
        pollfd p{listen_fd, POLLIN, 0};
        if (::poll(&p, 1, 50) <= 0) continue;
        const int c = ::accept(listen_fd, nullptr, nullptr);
        if (c < 0) continue;
        char tmp[8192];
        timeval tv{2, 0};
        ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        std::string buf;
        while (buf.find("\r\n\r\n") == std::string::npos) {
            const ssize_t r = ::recv(c, tmp, sizeof tmp, 0);
            if (r <= 0) break;
            buf.append(tmp, static_cast<size_t>(r));
        }
        accepted.fetch_add(1, std::memory_order_release);
        const std::string head = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n";
        ::send(c, head.data(), head.size(), MSG_NOSIGNAL);
        for (int i = 0; i < 300 && !stop.load(std::memory_order_acquire); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ::close(c);
    }
}

static Json args() { return dd::core::json_obj({{"q", std::make_shared<Json>(dd::core::json_str("x"))}}); }

int main() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    Server srv;
    if (!srv.start()) {
        std::printf("FAIL cannot start the test server\n");
        return 1;
    }
    auto setup = [&](MCPClient& c) {
        c.url = srv.url("/mcp");
        c.timeout_ms = 5000;
    };
    auto call = [&](Mode m, MCPClient& c) {
        srv.mode.store(m, std::memory_order_release);
        return c.call_ex("echo", args());
    };

    {   // every reply shape that carries the answer
        const struct { Mode m; const char* label; const char* want; } shapes[] = {
            {kSse, "sse", "hello"}, {kBare, "bare json, one line", "hello"}, {kPretty, "bare json, pretty-printed", "hello"},
            {kBatch, "json-rpc batch: the id that matches wins", "hello"}, {kWrongId, "sse: notification and wrong id skipped", "right"},
            {kMultiline, "sse: data over two lines", "joined"}, {kCrlf, "sse: CRLF lines", "crlf"}};
        for (const auto& s : shapes) {
            MCPClient c;
            setup(c);
            const auto r = call(s.m, c);
            chk(r.ok && !r.is_error && r.text == s.want, std::string("shape: ") + s.label + " (got '" + r.text + "' / " + r.error + ")");
        }
        MCPClient c;
        setup(c);
        srv.mode.store(kSse);
        chk(c.call("echo", args()) == "hello", "call() returns the plain text");
    }

    {   // a JSON-RPC error is a failure, everywhere
        MCPClient c;
        setup(c);
        const auto r = call(kRpcError, c);
        chk(!r.ok && has(r.error, "nope") && has(r.error, "-32601"), "rpc error -> ok=false with code and message: " + r.error);
        const std::string j = c.call("echo", args());
        const Json p = dd::core::json_parse(j);
        const Json* e = dd::core::json_get(p, "error");
        const Json* ie = dd::core::json_get(p, "isError");
        chk(e && e->type == Json::Str && has(e->str, "nope") && ie && ie->type == Json::Bool && ie->b, "call() on an rpc error is {error,isError}: " + j);
        bool threw = false;
        try {
            c.rpc("tools/call", dd::core::json_obj());
        } catch (const std::exception& ex) {
            threw = has(ex.what(), "nope");
        }
        chk(threw, "rpc() throws with the server's message on a JSON-RPC error");
    }

    {   // the tool itself says isError
        MCPClient c;
        setup(c);
        const auto r = call(kIsError, c);
        chk(r.ok && r.is_error && r.text == "boom" && r.error == "boom", "isError result keeps text and flags it");
        const Json p = dd::core::json_parse(c.call("echo", args()));
        const Json* ie = dd::core::json_get(p, "isError");
        chk(ie && ie->b, "call() on isError is {error,isError}");
    }

    {   // HTTP error without an answer, empty body, structured content, resource part
        MCPClient c;
        setup(c);
        auto r = call(kHttp500, c);
        chk(!r.ok && has(r.error, "HTTP 500") && has(r.error, "upstream exploded"), "HTTP 500 is a failure: " + r.error);
        r = call(kEmpty, c);
        chk(!r.ok && has(r.error, "no JSON-RPC answer"), "an empty 200 is not an answer: " + r.error);
        r = call(kStructured, c);
        chk(r.ok && r.text == "{\"n\":3}", "structuredContent stands in for empty text: " + r.text);
        r = call(kResource, c);
        chk(r.ok && r.text == "from a resource", "resource text is kept, an image part is not text: " + r.text);
    }

    {   // response cap
        MCPClient c;
        setup(c);
        c.max_bytes = 1024;
        const auto r = call(kBig, c);
        chk(!r.ok && has(r.error, "response over 1024 bytes"), "reply past max_bytes aborts: " + r.error);
        MCPClient d;
        setup(d);
        chk(call(kBig, d).ok == false, "64 KiB of junk without a JSON answer is a failure, not a hang");
    }

    {   // ids come from a counter; the matching one is picked
        MCPClient c;
        setup(c);
        const int base = srv.served.load();
        call(kSse, c);
        call(kSse, c);
        call(kSse, c);
        srv.wait_served(base + 3);
        long long last = -1;
        bool inc = true;
        for (int i = base; i < base + 3; ++i) {
            const Json j = dd::core::json_parse(srv.req(i).body);
            const Json* id = dd::core::json_get(j, "id");
            const long long v = id ? static_cast<long long>(id->num) : -1;
            inc = inc && v > last;
            last = v;
        }
        chk(inc && last > 1, "request ids increase");
    }

    {   // initialize: version + client info, session id kept and echoed, initialized notification, instructions
        MCPClient c;
        setup(c);
        srv.mode.store(kInit);
        const int base = srv.served.load();
        const std::string ins = c.instructions();
        chk(ins == "be nice", "instructions() reads the initialize result: '" + ins + "'");
        const auto r = c.call_ex("echo", args());
        chk(r.ok && r.text == "ok-session", "call after initialize works");
        srv.wait_served(base + 3);
        const Json init = dd::core::json_parse(srv.req(base).body);
        const Json* p = dd::core::json_get(init, "params");
        const Json* ci = p ? dd::core::json_get(*p, "clientInfo") : nullptr;
        const Json* nm = ci ? dd::core::json_get(*ci, "name") : nullptr;
        const Json* pv = p ? dd::core::json_get(*p, "protocolVersion") : nullptr;
        chk(nm && nm->str == "daedalus-cpp" && pv && pv->str == "2025-06-18" && p && dd::core::json_get(*p, "capabilities"),
            "initialize params carry protocolVersion, capabilities, clientInfo");
        chk(!srv.req(base).has_header("mcp-session-id"), "no session id before the server gives one");
        chk(has(srv.req(base + 1).body, "notifications/initialized") && srv.req(base + 1).header("mcp-session-id") == "sess-123",
            "notifications/initialized follows, with the session id");
        chk(srv.req(base + 2).header("mcp-session-id") == "sess-123" && srv.req(base + 2).header("mcp-protocol-version") == "2025-06-18",
            "later requests echo the session id and protocol version");
        // a stateless client never sends either
        MCPClient fresh;
        setup(fresh);
        const int b2 = srv.served.load();
        fresh.call_ex("echo", args());
        srv.wait_served(b2 + 1);
        chk(!srv.req(b2).has_header("mcp-session-id") && !srv.req(b2).has_header("mcp-protocol-version"), "a client that never initialized sends no handshake headers");
    }

    {   // tools/list follows nextCursor
        MCPClient c;
        setup(c);
        srv.mode.store(kPages);
        const Json t = c.list_tools();
        chk(t.type == Json::Arr && t.arr.size() == 2 && dd::core::json_get(*t.arr[0], "name")->str == "a" &&
                dd::core::json_get(*t.arr[1], "name")->str == "b",
            "list_tools reads every page");
    }

    {   // headers: env expansion, override of defaults, refusal of header injection
        setenv("DD_MCP_TEST_TOKEN", "abc123", 1);
        unsetenv("DD_MCP_TEST_UNSET_XYZ");
        MCPClient c;
        setup(c);
        c.headers = {{"Authorization", "Bearer ${DD_MCP_TEST_TOKEN}"}, {"X-Plain", "1"}, {"X-Empty", "${DD_MCP_TEST_UNSET_XYZ}"}, {"Accept", "application/json"}};
        const int base = srv.served.load();
        srv.mode.store(kSse);
        c.call_ex("echo", args());
        srv.wait_served(base + 1);
        const Req& rq = srv.req(base);
        chk(rq.header("authorization") == "Bearer abc123", "Authorization is expanded: '" + rq.header("authorization") + "'");
        chk(rq.header("x-plain") == "1" && !rq.has_header("x-empty"),
            "plain header kept; an unset env expansion drops the header (libcurl never sends an empty-valued one)");
        chk(rq.header("accept") == "application/json", "a configured Accept replaces the default");
        MCPClient bad;
        setup(bad);
        bad.headers = {{"Bad:Name", "x"}};
        auto r = bad.call_ex("echo", args());
        chk(!r.ok && has(r.error, "bad header"), "a header name with a colon is refused");
        bad.headers = {{"X-A", "v\r\nX-Injected: 1"}};
        r = bad.call_ex("echo", args());
        chk(!r.ok && has(r.error, "bad header"), "CRLF in a header value is refused");
        setenv("DD_MCP_TEST_X", "1", 1);
        chk(MCPClient::expand_env("a${DD_MCP_TEST_X}b") == "a1b", "expand_env: a variable");
        chk(MCPClient::expand_env("${DD_MCP_TEST_UNSET_XYZ}") == "", "expand_env: unset is empty");
        chk(MCPClient::expand_env("${}") == "${}" && MCPClient::expand_env("${A-B}") == "${A-B}" &&
                MCPClient::expand_env("$DD_MCP_TEST_X") == "$DD_MCP_TEST_X" && MCPClient::expand_env("${DD_MCP_TEST_X") == "${DD_MCP_TEST_X",
            "expand_env: anything that is not ${NAME} stays as written");
    }

    {   // transport: protocols, refused connection, never a throw from call_ex
        MCPClient c;
        c.timeout_ms = 3000;
        c.url = "file:///etc/hostname";
        auto r = c.call_ex("echo", args());
        chk(!r.ok && !r.error.empty(), "file:// is refused: " + r.error);
        c.url = "ftp://127.0.0.1:1/x";
        r = c.call_ex("echo", args());
        chk(!r.ok && !r.error.empty(), "ftp:// is refused: " + r.error);
        c.url = "http://127.0.0.1:1/mcp";
        bool threw = false;
        try {
            r = c.call_ex("echo", args());
        } catch (...) {
            threw = true;
        }
        chk(!threw && !r.ok && !r.error.empty(), "call_ex on a refused connection: no throw, ok=false: " + r.error);
        const Json p = dd::core::json_parse(c.call("echo", args()));
        chk(dd::core::json_get(p, "error") && dd::core::json_get(p, "isError"), "call() on a refused connection is {error,isError}");
        threw = false;
        try {
            c.instructions();
        } catch (const std::exception&) {
            threw = true;
        }
        chk(threw, "instructions() still throws on a transport failure (dd_tui boot relies on it)");
        threw = false;
        try {
            c.list_tools();
        } catch (const std::exception&) {
            threw = true;
        }
        chk(threw, "list_tools() still throws on a transport failure");
    }

    {   // net::get: cap, default cap, protocols
        auto r = dd::net::get(srv.url("/small"), 3000);
        chk(r.ok() && r.body == "ok", "net::get small");
        r = dd::net::get(srv.url("/big"), 3000, 1024);
        chk(!r.ok() && r.error == "response over 1024 bytes", "net::get past max_bytes: " + r.error);
        r = dd::net::get(srv.url("/big"), 3000);
        chk(r.ok() && r.body.size() == 64 * 1024, "net::get under the default cap");
        r = dd::net::get("file:///etc/hostname", 3000);
        chk(!r.ok() && !r.error.empty(), "net::get refuses file://");
        r = dd::net::post(srv.url("/small"), "{}", 3000, nullptr, 1);
        chk(!r.ok() && has(r.error, "response over 1 bytes"), "net::post honours max_bytes");
    }

    {   // the LLM transport: a stream, a refused protocol
        dd::agent::HttpLLM llm;
        llm.base_url = srv.base() + "/v1";
        llm.model = "m";
        std::string seen;
        llm.on_content = [&](const std::string& d) { seen += d; };
        dd::core::Message u;
        u.role = "user";
        u.content = "x";
        srv.mode.store(kLlmStream);
        const auto a = llm.chat({u}, dd::core::json_arr());
        chk(a.content == "hi" && seen == "hi" && a.finish_reason == "stop", "HttpLLM streams over the new options");
        llm.base_url = "file:///x";
        bool threw = false;
        try {
            llm.chat({u}, dd::core::json_arr());
        } catch (const std::exception&) {
            threw = true;
        }
        chk(threw, "HttpLLM refuses a file:// base url");
    }

    {   // a stream that goes quiet ends after stall_timeout_s, not after a fixed total time
        int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof a);
        ::listen(lfd, 4);
        socklen_t len = sizeof a;
        ::getsockname(lfd, reinterpret_cast<sockaddr*>(&a), &len);
        std::atomic<bool> stop{false};
        std::atomic<int> accepted{0};
        std::thread th(run_stall_server, lfd, std::ref(stop), std::ref(accepted));
        dd::agent::HttpLLM llm;
        llm.base_url = "http://127.0.0.1:" + std::to_string(ntohs(a.sin_port)) + "/v1";
        llm.model = "m";
        llm.stall_timeout_s = 1;
        dd::core::Message u;
        u.role = "user";
        u.content = "x";
        const auto t0 = std::chrono::steady_clock::now();
        std::string why;
        try {
            llm.chat({u}, dd::core::json_arr());
        } catch (const std::exception& e) {
            why = e.what();
        }
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        chk(!why.empty() && s >= 0.9 && s < 5.0 && accepted.load() == 1, "a silent stream ends after stall_timeout_s (took " + std::to_string(s) + " s: " + why + ")");
        stop.store(true);
        th.join();
        ::close(lfd);
    }

    if (g_fail) std::printf("%d FAILED\n", g_fail);
    else std::printf("all mcp/net tests passed\n");
    return g_fail ? 1 : 0;
}
