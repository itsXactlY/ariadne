#pragma once
// dd/net/server.hpp — a small HTTP/1.1 server for `ariadne serve`.
//
// One thread accepts; every connection gets its own thread and owns its socket,
// so nothing is shared but a count of live connections. Every response closes
// the connection (no keep-alive): a client of an agent sends one request per
// turn, and an SSE stream ends when the turn does.
//
// TLS is optional and per server: set_tls() hands it an OpenSSL SSL_CTX, and every
// connection then handshakes on its own thread before a byte of HTTP is read. A build
// without OpenSSL (-DDD_NO_TLS, the Android NDK one) keeps the plain server only.

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dd {
namespace net {

struct Request {
    std::string method, target, path, query, body;
    std::string peer;   // the client's address as text ("127.0.0.1", "::1"), "" when unknown
    bool tls = false;   // arrived over the TLS listener
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
    explicit Conn(int fd, void* ssl = nullptr) : fd_(fd), ssl_(ssl) {}
    Conn(const Conn&) = delete;
    Conn& operator=(const Conn&) = delete;
    ~Conn();

    bool respond(int status, std::string_view content_type, std::string_view body);
    bool json(int status, std::string_view body) { return respond(status, "application/json", body); }
    bool sse_begin();
    bool sse_data(std::string_view data);   // one `data: …` event
    // A response whose body is written as it arrives (a proxied event stream):
    // headers without a length, then raw bytes until the connection closes.
    bool stream_begin(int status, std::string_view content_type);
    bool stream_write(std::string_view bytes) { return write_all(bytes); }
    // false once a write failed: the peer is gone, stop producing for it.
    bool open() const { return ok_; }

private:
    bool write_all(std::string_view bytes);
    int fd_;
    void* ssl_;   // SSL* when the connection is TLS
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
    // Serve TLS on this listener: `ssl_ctx` is an SSL_CTX* the owner keeps alive
    // for the server's life. Ignored (returns false) in a build without TLS.
    bool set_tls(void* ssl_ctx);
    int port() const { return port_; }
    // Connections still answering a turn.
    int live() const { return rig_->live.load(std::memory_order_acquire); }
    // Accept until `stop` is non-zero, then wait for the live connections -- for a grace
    // of five seconds only. A handler that never returns would otherwise never let the
    // process exit; its thread is left running, and it holds what it needs by shared_ptr.
    void serve(const std::atomic<int>& stop);

private:
    // Everything a connection thread needs, behind a shared_ptr. A thread that outlives
    // serve() must not reach through the Server: its owner may have freed the Server while
    // the thread is still answering a turn that never honours the interrupt.
    struct Rig {
        Handler handler;
        size_t max_body;
        void* tls = nullptr;
        std::atomic<int> live{0};   // counted by the threads themselves, so a thread that
                                    // outlives serve() never decrements a freed Server
    };
    static void handle(int fd, const std::string& peer, std::shared_ptr<Rig> rig);
    std::string host_;
    int port_;
    std::shared_ptr<Rig> rig_;
    int listen_fd_ = -1;
};

const char* reason_phrase(int status);

}  // namespace net
}  // namespace dd
