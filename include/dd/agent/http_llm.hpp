#pragma once
// dd/agent/http_llm.hpp — the OpenAI chat/completions transport over libcurl.
// Calls stream (reasoning and answer handed out as they arrive); a set
// cancel flag aborts a call in flight. The request body is built in one
// place, so a prewarm sends exactly the bytes the next main call will.

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "dd/agent/agent.hpp"
#include "dd/core/json.hpp"
#include "dd/core/message.hpp"

namespace dd {
namespace agent {

struct CallStats {
    double latency_ms = 0, gen_tps = 0, pp_tps = 0;
    long prompt_tokens = 0, completion_tokens = 0, cached_tokens = 0;
};

// A message as the server reads it: reasoning goes out as
// reasoning_content, harness bookkeeping (finish_reason) stays home.
core::Json wire_message(const core::Message& m);

class HttpLLM : public LLMClient {
public:
    std::string base_url, model, api_key;
    const std::atomic<bool>* cancel = nullptr;
    std::function<std::string()> effort;          // reasoning_effort to send, "" = the server's default
    std::function<void(core::Json& body)> shape;  // endpoint fields (dd/context Endpoint)
    std::function<void()> on_begin;
    std::function<void(const std::string&)> on_reasoning, on_content;
    std::function<void(const CallStats&)> on_stats;
    bool streamed = false;   // the last call's answer went out through on_content
    long stall_timeout_s = 900;   // no bytes for this long ends a call (a long prefill on a slow box must survive)

    core::Message chat(const std::vector<core::Message>& messages, const core::Json& tools) override;

    core::Json body(const std::vector<core::Message>& messages, const core::Json& tools) const;
    // One unstreamed call that only fills the server's cache (max_tokens 1).
    bool prime(core::Json body, const std::atomic<bool>* cancel, long timeout_ms) const;
    std::string url() const;
};

}  // namespace agent
}  // namespace dd
