#pragma once
// dd/agent/http_llm.hpp — the OpenAI chat/completions transport over libcurl.
// Calls stream (reasoning and answer handed out as they arrive); a set
// cancel flag aborts a call in flight. The request body is built in one
// place, so a prewarm sends exactly the bytes the next main call will.
//
// A call is sent more than once only when the first send produced no answer at all
// (dd/agent/retry.hpp::wire_failure): a refused connect, a socket cut before the
// response line, a 5xx. Once any reasoning or answer byte has been handed out the call
// is never sent again — the caller would get the same words twice. retry_attempts = 1
// is the old one-shot behaviour; the send count rides out in CallStats and in the error.

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
    int attempts = 1;   // sends this call needed; more than one means the endpoint was slow
    // What the wire did, for the operator who asked to see it (/verbose verbose). A turn that
    // was rate-limited and came back is the same turn either way; only these numbers say so.
    long status = 0;         // the response line of the send that answered
    long first_status = 0;   // the status that started a re-send, 0 when the first send answered
    double waited_ms = 0, last_wait_ms = 0;   // paused between sends: the whole call, the last pause
    long retry_after_s = 0;  // the server's own Retry-After, when it named one
    std::string headers;     // the response headers worth reading, "name: value · ..."
};

// A message as the server reads it: reasoning goes out as
// reasoning_content, harness bookkeeping (finish_reason) stays home.
core::Json wire_message(const core::Message& m);

// ── App attribution ───────────────────────────────────────────────────────────
// A provider that files usage by app (OpenRouter) keys an app on the URL in
// HTTP-Referer: without that header the request lands in the anonymous bucket its model
// pages show as "Unknown" and no app page exists at all. X-OpenRouter-Title only names
// the app and X-OpenRouter-Categories only files it in the marketplace; neither creates
// an app on its own. openrouter.ai/docs/app-attribution is the contract.
struct Attribution {
    std::string referer;      // the app's URL: the identifier. empty = send no header at all
    std::string title;        // the display name in rankings
    std::string categories;   // comma-separated, only names OpenRouter documents (cli-agent ...)
};

// Ariadne's own identity: the public repository is the URL OpenRouter files the app under.
Attribution ariadne_attribution();

// True for an endpoint that keys usage on the Referer header — and so the only place these
// headers mean anything. A local llama-server has no app to be, and a referer sent to every
// host the harness talks to is a disclosure, not an identity.
bool files_usage_by_app(const std::string& base_url);

// What this run sends: `configured` when the operator named an app, else Ariadne's own when
// the endpoint files usage by app, else nothing (empty referer -> no headers on the wire).
Attribution attribution_for(const std::string& base_url, Attribution configured);

// `model.attribution` read as the operator's switch: false, off, none or "false" turns the
// headers off for every endpoint; absent or any other value leaves the policy above in charge.
bool attribution_off(const core::Json* node);

class HttpLLM : public LLMClient {
public:
    std::string base_url, model, api_key;
    // What the operator asked the upstream to carry, verbatim: a /v1 chat endpoint reads
    // fields this transport has no key of its own - one model is served by many providers,
    // and only a request field says which one answers. Merged before the harness's own
    // fields, so a name here can add a field but never replace one we decided.
    core::Json extra;
    const std::atomic<bool>* cancel = nullptr;
    std::function<std::string()> effort;          // reasoning_effort to send, "" = the server's default
    std::function<void(core::Json& body)> shape;  // endpoint fields (dd/context Endpoint)
    std::function<void()> on_begin;
    std::function<void(const std::string&)> on_reasoning, on_content;
    std::function<void(const CallStats&)> on_stats;
    bool streamed = false;   // the last call's answer went out through on_content
    // Who the upstream should file this traffic under: resolved once, at startup, by
    // attribution_for(). Empty referer means the request carries no attribution header.
    Attribution attribution;
    long stall_timeout_s = 900;   // no bytes for this long ends a call (a long prefill on a slow box must survive)
    // Sends for one call. 1 = never re-send. Nine is the rate ladder's length: three quick
    // re-sends (250 ms, 500 ms, 1 s) and then 1 s, 5 s, 15 s, 30 s, 1 min, so a limit that
    // clears inside a minute still answers the turn. A socket fact climbs the same count on a
    // millisecond scale and gives up in seconds.
    int retry_attempts = 9;
    // The wall clock one call may spend on sends *and* pauses it CHOSE. A count alone cannot bound
    // the wait. 150 s is the ladder's own sum with jitter; 0 = no budget. A wait the server named
    // is not this client's guess and is judged by retry_after_cap_ms below instead.
    double retry_budget_ms = 150000.0;
    // ... but a wait the SERVER named is not a guess, and refusing it ends the turn on information
    // we already have. An instructed Retry-After longer than the budget is obeyed up to this
    // ceiling (0 = no ceiling). 300 s is five minutes: past that the honest answer is that this
    // provider is exhausted, and the error says so with the number and the knob.
    double retry_after_cap_ms = 300000.0;
    // ... and a ceiling PER INSTRUCTION is not a bound on the call: nine sends that each obey a
    // 180 s instruction is twenty-seven minutes of one turn spent asleep, which is exactly what a
    // repeated rate limit produces. This is the whole allowance for instructed waiting in one call
    // (0 = no allowance: every instruction is then treated as past its room). 240 s honors one
    // 180 s window and refuses the second.
    double retry_after_total_ms = 240000.0;
    // A pause long enough to look like a dead program is announced: (status, retry_after_s, ms,
    // why) before it sleeps and (0, 0, 0, "") when it wakes. Any thread.
    std::function<void(long status, long retry_after_s, double ms, const std::string& why)> on_wait;

    core::Message chat(const std::vector<core::Message>& messages, const core::Json& tools) override;

    core::Json body(const std::vector<core::Message>& messages, const core::Json& tools) const;
    // One unstreamed call that only fills the server's cache (max_tokens 1).
    bool prime(core::Json body, const std::atomic<bool>* cancel, long timeout_ms) const;
    std::string url() const;
    // The attribution headers this call puts on the wire, in order; empty when unnamed.
    std::vector<std::string> attribution_headers() const;
};

}  // namespace agent
}  // namespace dd
