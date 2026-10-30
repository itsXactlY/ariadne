#pragma once
// dd/agent/chat_stream.hpp — an OpenAI chat.completion.chunk SSE stream
// (stream: true) folded into one assistant Message. Reasoning and answer
// deltas are handed out as they arrive, so a UI can follow both live.
// llama-server and Strata put the thinking in delta.reasoning_content, and
// usage + timings in the last chunk.

#include <cstddef>
#include <functional>
#include <map>
#include <string>

#include "dd/core/json.hpp"
#include "dd/core/message.hpp"

namespace dd {
namespace agent {

class ChatStream {
public:
    std::function<void(const std::string&)> on_reasoning, on_content;
    size_t max_line = 64u << 20;   // a line with no newline past this ends the stream with an error

    void feed(const char* p, size_t n);   // raw bytes, any split
    void finish();                        // flush a last line without "\n"

    bool saw_data() const { return chunks_ > 0; }
    core::Message message() const;        // role, content, tool_calls, finish_reason
    const std::string& reasoning() const { return reasoning_; }
    const core::Json& usage() const { return usage_; }
    const core::Json& timings() const { return timings_; }
    const std::string& error() const { return error_; }   // an in-stream {"error":…}, or an overlong line
    bool overflowed() const { return overflow_; }         // stop feeding: error() says why
    // The stream said the turn is over: a `data: [DONE]` line arrived. A server that keeps
    // the connection open afterwards (HTTP/1.1 keep-alive — llama-server, vLLM, OpenAI all
    // do) is not still answering, so a reader that waits for the socket instead of this
    // flag waits for a byte that will never come.
    bool done() const { return done_; }
    const std::string& raw_head() const { return head_; } // first bytes, for reports

private:
    void line(const std::string& l);

    std::string buf_, head_, content_, reasoning_, finish_, error_;
    std::map<long, core::ToolCall> calls_;   // by delta index
    core::Json usage_, timings_;
    long chunks_ = 0;
    bool overflow_ = false;
    bool done_ = false;
};

}  // namespace agent
}  // namespace dd
