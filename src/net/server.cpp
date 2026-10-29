// src/net/server.cpp — impl of dd/net/server.hpp.

#include "dd/net/server.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace dd {
namespace net {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

}  // namespace

const char* reason_phrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default: return "Status";
    }
}

std::string Request::header(std::string_view name) const {
    for (const auto& [k, v] : headers)
        if (k == name) return v;
    return "";
}

Parse parse_request(std::string_view raw, Request& out, size_t max_body) {
    const size_t head_end = raw.find("\r\n\r\n");
    if (head_end == std::string_view::npos) return raw.size() > (64u << 10) ? Parse::Bad : Parse::Incomplete;
    std::string_view head = raw.substr(0, head_end);

    const size_t line_end = head.find("\r\n");
    std::string_view line = head.substr(0, line_end);
    const size_t sp1 = line.find(' ');
    const size_t sp2 = sp1 == std::string_view::npos ? sp1 : line.find(' ', sp1 + 1);
    if (sp1 == std::string_view::npos || sp2 == std::string_view::npos) return Parse::Bad;
    Request r;
    r.method = std::string(line.substr(0, sp1));
    r.target = std::string(line.substr(sp1 + 1, sp2 - sp1 - 1));
    if (line.substr(sp2 + 1).rfind("HTTP/1.", 0) != 0 || r.target.empty() || r.target[0] != '/') return Parse::Bad;
    const size_t q = r.target.find('?');
    r.path = r.target.substr(0, q);
    if (q != std::string::npos) r.query = r.target.substr(q + 1);

    size_t content_length = 0;
    std::string_view rest = line_end == std::string_view::npos ? std::string_view{} : head.substr(line_end + 2);
    while (!rest.empty()) {
        const size_t e = rest.find("\r\n");
        std::string_view h = rest.substr(0, e);
        rest = e == std::string_view::npos ? std::string_view{} : rest.substr(e + 2);
        const size_t colon = h.find(':');
        if (colon == std::string_view::npos || colon == 0) return Parse::Bad;
        std::string name = lower(trim(h.substr(0, colon)));
        std::string value(trim(h.substr(colon + 1)));
        if (name == "content-length") {
            if (value.empty() || value.size() > 12) return Parse::Bad;
            for (char c : value)
                if (!std::isdigit(static_cast<unsigned char>(c))) return Parse::Bad;
            content_length = std::stoull(value);
        } else if (name == "transfer-encoding") {
            return Parse::Bad;   // chunked request bodies: no agent client sends them
        }
        r.headers.emplace_back(std::move(name), std::move(value));
    }
    if (content_length > max_body) return Parse::TooLarge;
    const size_t body_at = head_end + 4;
    if (raw.size() < body_at + content_length) return Parse::Incomplete;
    r.body = std::string(raw.substr(body_at, content_length));
    out = std::move(r);
    return Parse::Done;
}

Conn::~Conn() {
    if (fd_ >= 0) {
        ::shutdown(fd_, SHUT_WR);
        ::close(fd_);
    }
}

bool Conn::write_all(std::string_view bytes) {
    while (ok_ && !bytes.empty()) {
        const ssize_t n = ::send(fd_, bytes.data(), bytes.size(), MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok_ = false; break; }
        bytes.remove_prefix(static_cast<size_t>(n));
    }
    return ok_;
}

bool Conn::respond(int status, std::string_view content_type, std::string_view body) {
    if (headers_sent_) return false;
    headers_sent_ = true;
    std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason_phrase(status) + "\r\n";
    head += "Content-Type: " + std::string(content_type) + "\r\n";
    head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    head += "Connection: close\r\n\r\n";
    return write_all(head) && write_all(body);
}

bool Conn::sse_begin() {
    if (headers_sent_) return false;
    headers_sent_ = true;
    return write_all("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                     "Cache-Control: no-cache\r\nConnection: close\r\n\r\n");
}

bool Conn::sse_data(std::string_view data) {
    std::string ev;
    ev.reserve(data.size() + 8);
    ev += "data: ";
    ev += data;
    ev += "\n\n";
    return write_all(ev);
}

Server::Server(std::string host, int port, Handler handler, size_t max_body)
    : host_(std::move(host)), port_(port), handler_(std::move(handler)), max_body_(max_body) {}

Server::~Server() {
    if (listen_fd_ >= 0) ::close(listen_fd_);
}

bool Server::listen(std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
    addrinfo* res = nullptr;
    const std::string port = std::to_string(port_);
    if (const int rc = ::getaddrinfo(host_.c_str(), port.c_str(), &hints, &res); rc != 0) {
        error = std::string("resolve ") + host_ + ": " + gai_strerror(rc);
        return false;
    }
    for (addrinfo* a = res; a; a = a->ai_next) {
        const int fd = ::socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
        if (fd < 0) continue;
        const int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (::bind(fd, a->ai_addr, a->ai_addrlen) == 0 && ::listen(fd, 16) == 0) {
            listen_fd_ = fd;
            sockaddr_storage bound{};
            socklen_t len = sizeof bound;
            if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) == 0)
                port_ = ntohs(bound.ss_family == AF_INET6
                                  ? reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port
                                  : reinterpret_cast<sockaddr_in*>(&bound)->sin_port);
            break;
        }
        error = std::string("bind ") + host_ + ":" + port + ": " + std::strerror(errno);
        ::close(fd);
    }
    ::freeaddrinfo(res);
    if (listen_fd_ < 0 && error.empty()) error = "no address to bind for " + host_;
    return listen_fd_ >= 0;
}

void Server::serve(const std::atomic<int>& stop) {
    while (stop.load(std::memory_order_relaxed) == 0) {
        pollfd p{listen_fd_, POLLIN, 0};
        const int rc = ::poll(&p, 1, 200);
        if (rc <= 0 || !(p.revents & POLLIN)) continue;
        const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) continue;
        live_.fetch_add(1, std::memory_order_acq_rel);
        std::thread([this, fd] {
            handle(fd);
            live_.fetch_sub(1, std::memory_order_acq_rel);
        }).detach();
    }
    // A connection still streaming a turn ends when the turn does; the owner
    // interrupts the agent before it sets `stop`.
    while (live_.load(std::memory_order_acquire) > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

void Server::handle(int fd) {
    Conn conn(fd);
    timeval tv{30, 0};   // a client that stops sending mid-request does not hold a thread
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    std::string raw;
    char buf[16384];
    Request req;
    for (;;) {
        const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        raw.append(buf, static_cast<size_t>(n));
        switch (parse_request(raw, req, max_body_)) {
            case Parse::Incomplete: continue;
            case Parse::Bad: conn.json(400, R"({"error":{"message":"malformed request"}})"); return;
            case Parse::TooLarge: conn.json(413, R"({"error":{"message":"request body too large"}})"); return;
            case Parse::Done: break;
        }
        break;
    }
    try {
        handler_(req, conn);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "ariadne serve: %s %s: %s\n", req.method.c_str(), req.path.c_str(), ex.what());
        conn.json(500, R"({"error":{"message":"internal error, see the server log"}})");
    }
}

}  // namespace net
}  // namespace dd
