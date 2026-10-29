#pragma once
// dd/serve/chat_api.hpp — the OpenAI-compatible face of `ariadne serve`.
//
// Every consumer of an agent (the Mazemaker agent bridge, the phone app, the
// LAN gateway) already speaks /v1/chat/completions, so Ariadne answers in that
// shape. The harness owns the conversation — its tape, and Mazemaker for what
// ages out — so a request's last user message is the turn; the history a
// client sends along is not replayed.
//
// One agent runs one turn at a time. A second request while a turn runs is
// refused with 409: the claim is a compare-exchange, nothing waits on a lock.
// The agent's hooks fire on the thread that runs the turn, which is the
// request's own connection thread, so the stream they write to is that thread's.

#include <atomic>
#include <string>

#include "dd/net/server.hpp"

namespace dd {
namespace serve {

// What runs a turn: the assembled agent, or a fake in a test.
struct TurnRunner {
    virtual ~TurnRunner() = default;
    virtual std::string run(const std::string& user_message) = 0;
    virtual void interrupt() = 0;
};

struct ApiInfo {
    std::string model_id = "ariadne";   // what /v1/models lists and chunks name
    std::string upstream_model;          // the model the harness talks to
    std::string home;
    int tools = 0;
};

class ChatApi {
public:
    ChatApi(ApiInfo info, std::string api_key, TurnRunner& runner);

    void handle(const net::Request& req, net::Conn& conn);

    // Hooks the owner wires into the LLM transport and the agent loop.
    void on_content(const std::string& delta);     // streamed answer tokens
    void on_reasoning(const std::string& delta);   // streamed reasoning tokens
    void on_message(const std::string& content);   // an answer that was not streamed
    void on_tool_call(const std::string& id, const std::string& name, const std::string& arguments);
    void on_tool_result(const std::string& id, const std::string& content);

    bool busy() const { return busy_.load(std::memory_order_acquire); }

    static constexpr size_t kToolResultMax = 4000;   // a tool result travels this far, the rest is cut

private:
    bool authorized(const net::Request& req) const;
    void chat(const net::Request& req, net::Conn& conn);
    void emit(const std::string& delta_json);   // one chat.completion.chunk around a delta object
    std::string chunk(const std::string& delta_json, const char* finish) const;

    ApiInfo info_;
    std::string api_key_;
    TurnRunner& runner_;
    std::atomic<bool> busy_{false};
    // Set only while a streaming turn runs, read only on that turn's thread.
    std::atomic<net::Conn*> stream_{nullptr};
    std::string turn_id_;
    long created_ = 0;
    unsigned long turns_ = 0;   // the turn id stays unique within one second
    std::string streamed_text_;   // what reached the client, for the non-stream answer
};

}  // namespace serve
}  // namespace dd
