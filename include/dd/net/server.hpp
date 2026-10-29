#pragma once
// dd/net/server.hpp — a small HTTP/1.1 server for `ariadne serve`.
//
// One thread accepts; every connection gets its own thread and owns its socket,
// so nothing is shared but a count of live connections. Every response closes
// the connection (no keep-alive): a client of an agent sends one request per
// turn, and an SSE stream ends when the turn does.

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dd {
namespace net {

struct Request {
    std::string method, target, path, query, body;
    std::vector<std::pair<std::string, std::string>> headers;   // names lowercased
    // The first header of that (lowercase) name, "" when absent.
    std::string header(std::string_view name) const;
};

enum class Parse { Incomplete, Done, Bad, TooLarge };

// One request out of the bytes read so far. Incomplete wants more bytes; a body
// past max_body is TooLarge (413), anything malformed is Bad (400).
Parse parse_request(std::string_view raw, Request& out, size_t max_body);

class Conn {
public:
    explicit Conn(int fd) : fd_(fd) {}
    Conn(const Conn&) = delete;
    Conn& operator=(const Conn&) = delete;
    ~Conn();

    bool respond(int status, std::string_view content_type, std::string_view body);
    bool json(int status, std::string_view body) { return respond(status, "application/json", body); }
    bool sse_begin();
    bool sse_data(std::string_view data);   // one `data: …` event
    // false once a write failed: the peer is gone, stop producing for it.
    bool open() const { return ok_; }

private:
    bool write_all(std::string_view bytes);
    int fd_;
    bool ok_ = true;
    bool headers_sent_ = false;
};

using Handler = std::function<void(const Request&, Conn&)>;

class Server {
public:
    Server(std::string host, int port, Handler handler, size_t max_body = 4u << 20);
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    ~Server();

    bool listen(std::string& error);   // bind + listen; port 0 picks a free one
    int port() const { return port_; }
    // Accept until `stop` is non-zero, then wait for the live connections.
    void serve(const std::atomic<int>& stop);

private:
    void handle(int fd);
    std::string host_;
    int port_;
    Handler handler_;
    size_t max_body_;
    int listen_fd_ = -1;
    std::atomic<int> live_{0};
};

const char* reason_phrase(int status);

}  // namespace net
}  // namespace dd
