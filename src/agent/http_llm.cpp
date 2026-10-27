// src/agent/http_llm.cpp — impl of dd/agent/http_llm.hpp.
#include "dd/agent/http_llm.hpp"

#include "dd/agent/chat_stream.hpp"
#include "dd/agent/retry.hpp"
#include "dd/net/http.hpp"
#include "dd/tools/url_safety.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>

namespace dd {
namespace agent {

using core::Json;

namespace {

core::JsonSP jsp(Json v) { return std::make_shared<Json>(std::move(v)); }

double num_at(const Json& o, const char* a, const char* b = nullptr) {
    const Json* v = core::json_get(o, a);
    if (v && b) v = v->type == Json::Obj ? core::json_get(*v, b) : nullptr;
    return v && v->type == Json::Num ? v->num : 0.0;
}

// Stop the moment the flag is set; keep raw bytes until the first SSE chunk
// in case the server answers with plain JSON (an error, or no streaming).
// The sink also carries the stall clock: LOW_SPEED_* only counts bytes curl
// received, so a server that sends headers and then falls silent never trips
// it — this callback does.
struct StreamSink {
    ChatStream cs;
    std::string raw;
    const std::atomic<bool>* cancel = nullptr;
    curl_off_t recv = 0;    // bytes the write callback has seen
    long stall_s = 0;       // 0 = no stall guard
    curl_off_t last_recv = 0;
    bool seen = false;
    bool stalled = false;
    std::chrono::steady_clock::time_point since{};
    curl_off_t max_bytes = 64u << 20;   // hard cap on the whole response
    curl_off_t total = 0;
    bool over = false;
    bool spoke = false;   // a non-empty delta reached the caller: this call can never be sent again
};

int abort_if(void* ud, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* s = static_cast<StreamSink*>(ud);
    if (s->cancel && s->cancel->load(std::memory_order_relaxed)) return 1;
    if (s->stall_s <= 0) return 0;
    const auto now = std::chrono::steady_clock::now();
    if (!s->seen) {
        s->seen = true;
        s->since = now;
        s->last_recv = s->recv;
        return 0;
    }
    if (s->recv != s->last_recv) {   // bytes are still moving: the clock resets
        s->last_recv = s->recv;
        s->since = now;
        return 0;
    }
    if (std::chrono::duration_cast<std::chrono::seconds>(now - s->since).count() >= s->stall_s) {
        s->stalled = true;
        return 1;
    }
    return 0;
}

std::size_t stream_sink(char* p, std::size_t, std::size_t n, void* ud) {
    auto* s = static_cast<StreamSink*>(ud);
    if (s->cancel && s->cancel->load(std::memory_order_relaxed)) return 0;
    if (s->total + static_cast<curl_off_t>(n) > s->max_bytes) {   // hard byte cap, like net::sink
        s->over = true;
        return 0;
    }
    s->total += static_cast<curl_off_t>(n);
    if (!s->cs.saw_data() && s->raw.size() < (1u << 24)) s->raw.append(p, n);
    s->cs.feed(p, n);
    return s->cs.overflowed() ? 0 : n;
}

// One streamed send, in the terms the retry policy reads. curl's code travels as an
// int so the policy needs no curl header.
WireResult wire_result(CURLcode rc, long status, const StreamSink& s, bool cancelled) {
    WireResult r;
    r.code = static_cast<int>(rc);
    r.status = status;
    r.stalled = s.stalled;
    r.cancelled = cancelled;
    r.spoken = s.spoke;
    r.refused = s.over || s.cs.overflowed();
    r.stream_error = !s.cs.error().empty();
    return r;
}

// net::Response, in the same terms: net answers rather than throwing, so a response
// line is an answer and anything else is a fact about the socket. A url the guard
// refused is the guard answering, not a hiccup.
WireResult wire_result(const net::Response& r) {
    WireResult w;
    w.code = r.status > 0 ? 0 : -1;   // -1: the server never answered
    w.status = r.status;
    w.cancelled = r.cancelled;
    w.refused = r.error == "url rejected";
    return w;
}

// Why this send failed, in exactly the words the caller got before the send loop existed.
std::string why(CURLcode rc, long status, StreamSink& s, bool cancelled) {
    if (cancelled) return "interrupted";
    if (s.stalled) return "curl: stalled — no bytes for " + std::to_string(s.stall_s) + "s";
    if (rc == CURLE_WRITE_ERROR && s.over) return "response over " + std::to_string(s.max_bytes) + " bytes";
    if (rc == CURLE_WRITE_ERROR && s.cs.overflowed()) return s.cs.error();
    if (rc != CURLE_OK) return std::string("curl: ") + curl_easy_strerror(rc);

    Json resp;
    if (!s.cs.saw_data()) { try { resp = core::json_parse(s.raw); } catch (...) {} }
    if (const Json* e = core::json_get(resp, "error")) {
        const Json* m = e->type == Json::Obj ? core::json_get(*e, "message") : nullptr;
        return "HTTP " + std::to_string(status) + ": " + (m && m->type == Json::Str ? m->str : core::json_dump(*e));
    }
    if (!s.cs.error().empty()) return "HTTP " + std::to_string(status) + ": " + s.cs.error();
    return "HTTP " + std::to_string(status) + ": " + (s.raw.empty() ? std::string("no body") : s.raw.substr(0, 400));
}

// Wait the backoff out in slices, so an Esc during the pause ends it inside one slice
// rather than after the whole wait — the loop's tool-delay idiom. False means the
// operator cut the call while it was waiting.
bool wait_backoff(double ms, const std::atomic<bool>* stop) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double, std::milli>(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (stop && stop->load(std::memory_order_acquire)) return false;
        const auto left = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - std::chrono::steady_clock::now());
        std::this_thread::sleep_for(std::min(left, std::chrono::duration<double, std::milli>(50.0)));
    }
    return !(stop && stop->load(std::memory_order_acquire));
}

// A slow endpoint is visible instead of mysterious: the reply says how many sends it took.
std::string with_sends(const std::string& text, int sends) {
    return sends > 1 ? text + " (after " + std::to_string(sends) + " sends)" : text;
}

}  // namespace

Json wire_message(const core::Message& m) {
    Json o = core::message_to_json(m);
    for (auto it = o.obj.begin(); it != o.obj.end();) {
        if (it->first == "finish_reason") {
            it = o.obj.erase(it);
            continue;
        }
        if (it->first == "reasoning") it->first = "reasoning_content";
        ++it;
    }
    return o;
}

std::string HttpLLM::url() const {
    std::string u = base_url;
    if (!u.empty() && u.back() != '/') u += "/";
    return u + "chat/completions";
}

Json HttpLLM::body(const std::vector<core::Message>& messages, const Json& tools) const {
    Json b = core::json_obj();
    core::obj_set(b, "model", core::json_str(model));
    Json msgs = core::json_arr();
    for (const auto& m : messages) core::arr_push(msgs, wire_message(m));
    core::obj_set(b, "messages", msgs);
    if (tools.type == Json::Arr && !tools.arr.empty()) core::obj_set(b, "tools", tools);
    if (effort)
        if (const std::string e = effort(); !e.empty()) core::obj_set(b, "reasoning_effort", core::json_str(e));
    if (shape) shape(b);
    return b;
}

bool HttpLLM::prime(Json b, const std::atomic<bool>* stop, long timeout_ms) const {
    core::obj_set(b, "stream", core::json_bool(false));
    core::obj_set(b, "max_tokens", core::json_num(1));
    const std::string payload = core::json_dump(b);
    const std::string endpoint = url();
    const int sends = retry_attempts > 1 ? retry_attempts : 1;
    for (int attempt = 1; attempt <= sends; ++attempt) {
        const net::Response r = net::post(endpoint, payload, timeout_ms, stop);
        if (r.ok()) return true;
        // A cache fill that never landed is worth one more fill; a cut call, a refused
        // url and an answered status are all final — the same no comes back.
        if (!transient(wire_failure(wire_result(r))) || attempt == sends) return false;
        if (!wait_backoff(wire_backoff_ms(attempt), stop)) return false;
    }
    return false;
}

core::Message HttpLLM::chat(const std::vector<core::Message>& messages, const Json& tools) {
    Json b = body(messages, tools);
    core::obj_set(b, "stream", core::json_bool(true));
    core::obj_set(b, "stream_options", core::json_obj({{"include_usage", jsp(core::json_bool(true))}}));
    const std::string payload = core::json_dump(b);
    const std::string endpoint = url();
    if (!dd::tools::is_safe_url(endpoint)) throw std::runtime_error("url rejected: " + endpoint);

    const int sends = retry_attempts > 1 ? retry_attempts : 1;
    const auto t0 = std::chrono::steady_clock::now();   // the caller's wait: every send and every pause in it
    std::string last;   // the failure the caller finally gets, in curl's own words

    for (int attempt = 1; attempt <= sends; ++attempt) {
        StreamSink sink;
        sink.cancel = cancel;
        sink.stall_s = stall_timeout_s;
        // A delta that reached the caller marks this call as spoken: a second send of a
        // stream that already spoke would hand the same words out a second time.
        // Set only when the caller has a callback, exactly as before: ChatStream hands a
        // delta out only if a callback exists, so the wrapper marks the call spoken on the
        // one path a second send would duplicate.
        if (on_reasoning) sink.cs.on_reasoning = [&](const std::string& d) {
            if (!d.empty()) sink.spoke = true;
            on_reasoning(d);
        };
        if (on_content) sink.cs.on_content = [&](const std::string& d) {
            if (!d.empty()) sink.spoke = true;
            on_content(d);
        };

        CURL* c = curl_easy_init();
        if (!c) throw std::runtime_error("curl: out of memory for handle");
        struct CurlGuard { CURL* c; ~CurlGuard() { curl_easy_cleanup(c); } } guard{c};
        struct curl_slist* hdrs = curl_slist_append(nullptr, "Content-Type: application/json");
        if (!api_key.empty()) hdrs = curl_slist_append(hdrs, ("Authorization: Bearer " + api_key).c_str());
        struct SListGuard { struct curl_slist* h; ~SListGuard() { curl_slist_free_all(h); } } hguard{hdrs};

        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<curl_off_t>(payload.size()));
        curl_easy_setopt(c, CURLOPT_URL, endpoint.c_str());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, stream_sink);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);   // Esc aborts a call; this only guards a dead server
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, stall_timeout_s);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, abort_if);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, &sink);

        streamed = false;
        if (on_begin) on_begin();
        const CURLcode rc = curl_easy_perform(c);
        long status = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        // Only curl stopping counts as a cut: an Esc that lands after the answer arrived
        // still hands back that answer, exactly as it did before the send loop existed.
        const bool flag = cancel && cancel->load(std::memory_order_relaxed);
        const bool cancelled = (rc == CURLE_ABORTED_BY_CALLBACK && !sink.stalled) || (rc == CURLE_WRITE_ERROR && flag);

        const WireFailure f = wire_failure(wire_result(rc, status, sink, cancelled));
        if (f == WireFailure::None) {
            sink.cs.finish();
            Json resp;
            if (!sink.cs.saw_data()) {
                try { resp = core::json_parse(sink.raw); } catch (...) {}
            }

            core::Message assistant;
            if (sink.cs.saw_data()) {
                assistant = sink.cs.message();
                assistant.reasoning = sink.cs.reasoning();
                streamed = static_cast<bool>(on_content) && !assistant.content.empty();
                resp = core::json_obj({{"usage", jsp(sink.cs.usage())}, {"timings", jsp(sink.cs.timings())}});
            } else if (const Json* choices = core::json_get(resp, "choices");
                       choices && choices->type == Json::Arr && !choices->arr.empty() && choices->arr[0]) {
                const Json& ch0 = *choices->arr[0];
                if (const Json* msg = core::json_get(ch0, "message")) {
                    assistant = core::message_from_json(*msg);
                    if (const Json* r = core::json_get(*msg, "reasoning_content"); r && r->type == Json::Str && !r->str.empty()) {
                        assistant.reasoning = r->str;
                        if (on_reasoning) on_reasoning(r->str);
                    }
                }
                if (assistant.finish_reason.empty())
                    if (const Json* fr = core::json_get(ch0, "finish_reason"); fr && fr->type == Json::Str) assistant.finish_reason = fr->str;
            }
            if (assistant.role.empty()) assistant.role = "assistant";

            if (on_stats) {   // llama-server and Strata report their own speeds under "timings"
                CallStats st;
                st.attempts = attempt;   // more than one: the endpoint was slow enough that this turn re-sent
                st.latency_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                st.prompt_tokens = static_cast<long>(num_at(resp, "usage", "prompt_tokens"));
                st.completion_tokens = static_cast<long>(num_at(resp, "usage", "completion_tokens"));
                st.gen_tps = num_at(resp, "timings", "predicted_per_second");
                st.pp_tps = num_at(resp, "timings", "prompt_per_second");
                st.cached_tokens = static_cast<long>(num_at(resp, "timings", "cache_n"));
                if (const Json* u = core::json_get(resp, "usage"); u && u->type == Json::Obj && st.cached_tokens == 0)
                    st.cached_tokens = static_cast<long>(num_at(*u, "prompt_tokens_details", "cached_tokens"));
                on_stats(st);
            }
            return assistant;
        }

        last = why(rc, status, sink, cancelled);
        // Only a socket fact is sent again, and only while nothing has been handed out.
        // A cut call, a 4xx, a capped body, a stalled server and the last send all end here.
        if (!transient(f) || attempt == sends) throw std::runtime_error(with_sends(last, attempt));
        if (!wait_backoff(wire_backoff_ms(attempt), cancel)) throw std::runtime_error("interrupted");
    }

    throw std::runtime_error(with_sends(last, sends));   // unreachable: the final send throws above
}

}  // namespace agent
}  // namespace dd
