#pragma once
// dd/limbo/level.hpp — the limbo level's front: one socket on one port, and the few paths a
// harness needs to get a turn answered.
//
// Everything the Python endpoint spoke is here, and nothing else: /health, /props, /slots,
// /metrics, /v1/models, /v1/chat/completions. No web dashboard — a dashboard is a browser, and
// the daemon has no browser (the harness has one, outside the daemon).
//
// A handler runs on a connection thread and does the turn itself. That is deliberate: a turn is
// one pipe read, and a thread inside a turn is a thread doing work. The level adds no thread
// pool of its own; the connections the listener accepts are its concurrency limit.
//
// A turn is never sent to a dweller that is not admitted: the level answers 409 with the state
// it found, a full slot budget answers 429, and an unadmitted model answers 404. All three are
// refusals a caller can act on; none of them is a retry loop inside the level.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <string>
#include <string_view>
#include <vector>

#include "dd/limbo/minos.hpp"
#include "dd/limbo/shade.hpp"
#include "dd/net/server.hpp"

namespace dd {
namespace limbo {

struct LimboConfig {
    std::vector<ShadeSpec> shades;   // order = preference: the first admitted dweller answers
    const Decoder* decoder = nullptr;  // the lexicon the strata dwellers speak with; null until P1
    // Who builds a dweller. Default null builds the real ones; a test injects a fake shade, so
    // the front and the judge are tested against the contract instead of against a 23 GB load.
    std::function<std::unique_ptr<Shade>(const ShadeSpec&)> build = nullptr;
    std::string bind = "127.0.0.1";  // loopback only, unless the operator opens it
    int port = 0;                    // 0: the kernel picks, and the level reports what it got
    long respawn_budget = 5;         // dweller restarts per run before this level gives up
    size_t max_body = 8u << 20;      // a transcript is a body; 8 MB is a long conversation
};

// What a handler can write. net::Conn implements it; a test captures it. Kept abstract so
// routing is testable without a socket, and so a handler never reaches through a Server.
class Writer {
public:
    virtual ~Writer() = default;
    virtual bool respond(int status, std::string_view content_type, std::string_view body) = 0;
    virtual bool stream_begin(int status, std::string_view content_type) = 0;
    virtual bool stream_write(std::string_view bytes) = 0;
    // false once the peer is gone: produce nothing more for it.
    virtual bool open() const = 0;

    bool json(int status, std::string_view body) { return respond(status, "application/json", body); }

    static core::Json error_json(const std::string& type, const std::string& message) {
        core::Json e = core::json_obj();
        core::obj_set(e, "type", core::json_str(type));
        core::obj_set(e, "message", core::json_str(message));
        core::Json p = core::json_obj();
        core::obj_set(p, "error", e);
        return p;
    }
};

// The real front: writes through a net::Conn.
class ConnWriter : public Writer {
public:
    explicit ConnWriter(net::Conn& conn) : conn_(conn) {}
    bool respond(int status, std::string_view content_type, std::string_view body) override {
        return conn_.respond(status, content_type, body);
    }
    bool stream_begin(int status, std::string_view content_type) override { return conn_.stream_begin(status, content_type); }
    bool stream_write(std::string_view bytes) override { return conn_.stream_write(bytes); }
    bool open() const override { return conn_.open(); }

private:
    net::Conn& conn_;
};

class LimboLevel {
public:
    explicit LimboLevel(LimboConfig cfg);
    ~LimboLevel();

    // Bring the dwellers up, then bind the front and serve on one thread. A dweller that fails
    // to load is reported and the level still starts: a fallback that works beats a level that
    // refuses to come up at all.
    bool start(std::string& error);
    void stop();

    int port() const { return port_; }
    const std::string& bind() const { return cfg_.bind; }
    Minos& minos() { return *minos_; }
    const Minos& minos() const { return *minos_; }
    bool listening() const { return front_ != nullptr; }
    int live() const { return front_ ? front_->live() : 0; }

    // The HTTP front. Runs on a connection thread; a test may call it directly with a capture.
    void route(const net::Request& req, Writer& out);

    // A Chat-Completions body into a turn. false with a 400-shaped reason.
    static bool parse_turn(const std::string& body, TurnRequest& out, std::string& error);

    // One non-streaming Chat-Completions answer for a turn result.
    static core::Json completion_json(const TurnResult& r, const std::string& model);

    // One streaming chunk, in the shape a Chat-Completions reader parses.
    static core::Json chunk_json(const Delta& d, const std::string& model, const std::string& finish);

    // The timings block, on its own: a streaming turn puts it in the last chunk, and a
    // non-streaming one puts it in the answer. Same numbers, one builder.
    static core::Json timings_json(const TurnResult& r);

    const LimboConfig& config() const { return cfg_; }

private:
    void completions(const net::Request& req, Writer& out);
    void models(Writer& out) const;

    LimboConfig cfg_;
    std::unique_ptr<Minos> minos_;
    std::unique_ptr<net::Server> front_;
    std::atomic<int> stop_{0};
    std::unique_ptr<std::thread> serving_;
    int port_ = 0;
};

}  // namespace limbo
}  // namespace dd
