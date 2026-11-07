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
#include <cctype>
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
    long retry_after_s = 0;   // the server's own Retry-After, read off the response headers
    std::vector<std::pair<std::string, std::string>> headers;   // the ones worth reading, in arrival order
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
    bool completed = false;   // the stream reached `data: [DONE]`: the turn is over, the socket may not be
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

// A header line, kept only when an operator could act on it. The transport's own plumbing
// (date, lengths, encodings, CORS) says nothing about the call; a rate limit's remaining quota,
// the request id to quote, Retry-After and the provider that answered all do. Name lowercased
// once, so a header repeated by a proxy is not shown twice.
void keep_header(StreamSink& s, const std::string& line) {
    const size_t c = line.find(':');
    if (c == std::string::npos) return;   // the status line, or the blank that ends a block
    std::string name;
    for (size_t i = 0; i < c; ++i) {
        const char ch = line[i];
        if (ch == ' ' || ch == '\t') continue;
        name.push_back(ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch);
    }
    static const char* const kPlumbing[] = {
        "date", "content-type", "content-length", "connection", "keep-alive", "transfer-encoding",
        "cache-control", "vary", "access-control-allow-origin", "access-control-expose-headers",
        "strict-transport-security", "x-content-type-options", "alt-svc", "set-cookie", "server-timing",
        "referrer-policy", "permissions-policy", "cross-origin-opener-policy",
        "cross-origin-resource-policy", "content-encoding", "etag"};
    for (const char* p : kPlumbing)
        if (name == p) return;
    std::string val = line.substr(c + 1);
    while (!val.empty() && (val.back() == '\n' || val.back() == '\r')) val.pop_back();
    const size_t b = val.find_first_not_of(" \t");
    val = b == std::string::npos ? std::string() : val.substr(b);
    if (val.empty() || val.size() > 120) return;
    for (const auto& [n, v] : s.headers)
        if (n == name) return;   // one line per name: the first block is the one that answered
    if (s.headers.size() >= 12) return;
    s.headers.emplace_back(name, val);
}

// Every response header line, once: a 429 or a 503 says how long to wait, and a client that
// guesses its own backoff instead comes back early and is refused again.
std::size_t stream_header_cb(char* p, std::size_t, std::size_t n, void* ud) {
    auto* s = static_cast<StreamSink*>(ud);
    const std::string line(p, n);
    const long secs = net::retry_after_seconds(line);
    if (secs > 0) s->retry_after_s = secs;
    keep_header(*s, line);
    return n;
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
    // The terminator is the answer's end, not the socket's. A keep-alive server holds the
    // connection open for the next request, so a reader that waits for EOF sits on it until
    // the stall clock fires — every turn costs the stall timeout and is filed as a stall.
    // Returning past the read count makes curl stop the transfer here.
    if (s->cs.done()) { s->completed = true; return 0; }
    return s->cs.overflowed() ? 0 : n;
}

// One streamed send, in the terms the retry policy reads. curl's code travels as an
// int so the policy needs no curl header.
WireResult wire_result(CURLcode rc, long status, const StreamSink& s, bool cancelled) {
    WireResult r;
    r.code = static_cast<int>(rc);
    r.status = status;
    r.retry_after_s = s.retry_after_s;
    r.stalled = s.stalled;
    r.cancelled = cancelled;
    r.spoken = s.spoke;
    r.refused = s.over || s.cs.overflowed();
    r.stream_error = !s.cs.error().empty();
    r.completed = s.completed;
    return r;
}

// net::Response, in the same terms: net answers rather than throwing, so a response
// line is an answer and anything else is a fact about the socket. A url the guard
// refused is the guard answering, not a hiccup.
WireResult wire_result(const net::Response& r) {
    WireResult w;
    w.code = r.status > 0 ? 0 : -1;   // -1: the server never answered
    w.status = r.status;
    w.retry_after_s = r.retry_after_s;
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

static double elapsed_ms(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// -1 means "no budget": the loop then retries on the attempt count alone. A spent budget is 0, not
// a negative that a caller could read as "unbounded" — the ladder must stop once there is no room.
static double budget_left_ms(std::chrono::steady_clock::time_point t0, double budget_ms) {
    return budget_ms > 0.0 ? std::max(0.0, budget_ms - elapsed_ms(t0)) : -1.0;
}

// A wait the server NAMED is knowledge, not a guess. The budget bounds the ladder — this client's
// own speculation about when a limit clears — and it must not throw away the server's answer to
// the same question. OpenRouter answering "429, Retry-After: 180" out of a 150 s budget ended the
// operator's very first prompt of a session; obeying the number it handed over is both cheaper
// than the wasted turn and the only reading of the sentence that is actually true. An instructed
// wait is therefore taken up to its own ceiling, and the budget governs waits nobody named.
static bool instructed_within_ceiling(long retry_after_s, double cap_ms) {
    if (retry_after_s <= 0) return false;
    return cap_ms <= 0.0 || static_cast<double>(retry_after_s) * 1000.0 <= cap_ms;
}

// Room this send may spend on a pause. Two allowances, because the two kinds of pause are not the
// same kind of thing: the operator's budget covers what THIS CLIENT decided to wait, and the
// instructed allowance covers what the SERVER named. A ceiling per instruction cannot bound a
// repeated rate limit -- nine sends that each obey a 180 s instruction is twenty-seven minutes of
// one turn spent asleep -- so the instructed side is capped in total as well. -1 = no room, and the
// caller stops with a reason instead of sleeping again.
static double wait_room(std::chrono::steady_clock::time_point t0, double budget_ms, long retry_after_s,
                        double cap_ms, double instructed_ms, double instructed_spent_ms) {
    if (retry_after_s > 0) {
        // 0 room, not -1: -1 already means "unbounded" to the ladder, and a refusal coded as
        // "unbounded" is the bug this helper exists to prevent.
        if (!instructed_within_ceiling(retry_after_s, cap_ms)) return 0.0;   // named, and too long
        if (instructed_ms <= 0.0) return 0.0;                               // no allowance at all
        return std::max(0.0, instructed_ms - instructed_spent_ms);
    }
    return budget_left_ms(t0, budget_ms);
}

// What the caller is told when the budget, not the attempt count, ran out. It has to say the
// two things the operator cannot see: how many sends were spent, and that the server named a
// wait this client would not sit through.
static std::string budget_spent(const std::string& text, int sends, long retry_after_s, double budget_ms,
                                double cap_ms, double instructed_ms, double instructed_spent_ms) {
    std::string s = with_sends(text, sends);
    const std::string secs = std::to_string(static_cast<long>(budget_ms / 1000.0));
    const std::string asked_s = std::to_string(retry_after_s);
    if (retry_after_s > 0 && !instructed_within_ceiling(retry_after_s, cap_ms))
        s += " (the server asked for " + asked_s + "s, past the " + secs +
             "s ceiling for an instructed wait: agent.retry.retry_after_cap_ms raises it)";
    else if (retry_after_s > 0 && instructed_ms <= 0.0)
        s += " (the server asked for " + asked_s +
             "s, and instructed waiting is switched off: agent.retry.retry_after_total_ms turns it on)";
    else if (retry_after_s > 0)
        s += " (the server asked for " + asked_s + "s again, and " +
             std::to_string(static_cast<long>(instructed_spent_ms / 1000.0)) + "s of instructed waiting of the " +
             std::to_string(static_cast<long>(instructed_ms / 1000.0)) +
             "s this call may spend on it is already gone: agent.retry.retry_after_total_ms raises it)";
    else
        s += " (the " + secs + "s retry budget was spent)";
    return s;
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

// ── App attribution ───────────────────────────────────────────────────────────
namespace {

// scheme://[user[:pass]@]host[:port][/path] -> "host", lowercased. "" when the URL names none.
std::string host_of(const std::string& url) {
    const size_t at = url.find("://");
    const size_t beg = at == std::string::npos ? 0 : at + 3;
    size_t end = url.find_first_of("/?#", beg);
    if (end == std::string::npos) end = url.size();
    std::string host = url.substr(beg, end - beg);
    if (const size_t c = host.rfind('@'); c != std::string::npos) host = host.substr(c + 1);
    if (const size_t c = host.find(':'); c != std::string::npos) host = host.substr(0, c);   // port, not IPv6
    for (char& ch : host) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return host;
}

// A header value is one line by construction: a CR or LF inside it would end our header and
// start one nobody wrote. The operator's own config is the source, so this is a guard, not a
// parser.
std::string header_safe(const std::string& v) {
    std::string o;
    o.reserve(v.size());
    for (const char c : v)
        if (c != '\r' && c != '\n') o += c;
    const size_t b = o.find_first_not_of(" \t");
    if (b == std::string::npos) return {};
    const size_t e = o.find_last_not_of(" \t");
    return o.substr(b, e - b + 1);
}

// The identifier is a URL or it is nothing: OpenRouter files by URL and drops anything else,
// so a value that is not http(s) is treated as "no app named" rather than sent.
bool is_url(const std::string& v) {
    const std::string l = [&] { std::string s = v; for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }();
    return l.rfind("http://", 0) == 0 || l.rfind("https://", 0) == 0;
}

}  // namespace

Attribution ariadne_attribution() {
    // The public repository, not a marketing subdomain: it is stable, it describes the project
    // on its own, and docs/Best-practices say to avoid subdomains that are not distinct apps.
    return {"https://github.com/itsXactlY/ariadne", "Ariadne", "cli-agent"};
}

bool files_usage_by_app(const std::string& base_url) {
    const std::string h = host_of(base_url);
    static const std::string kHost = "openrouter.ai";
    return h == kHost || (h.size() > kHost.size() && h.compare(h.size() - kHost.size(), kHost.size(), "." + kHost) == 0);
}

Attribution attribution_for(const std::string& base_url, Attribution configured) {
    if (is_url(configured.referer)) {
        if (configured.title.empty()) configured.title = "Ariadne";
        return configured;   // the operator named their app: it goes out wherever they point us
    }
    if (files_usage_by_app(base_url)) return ariadne_attribution();
    return {};
}

bool attribution_off(const Json* node) {
    if (!node) return false;
    if (node->type == Json::Bool) return !node->b;
    if (node->type != Json::Str) return false;
    const std::string v = header_safe(node->str);
    return v == "off" || v == "none" || v == "false" || v == "no";
}

std::vector<std::string> HttpLLM::attribution_headers() const {
    std::vector<std::string> h;
    const std::string ref = header_safe(attribution.referer);
    if (ref.empty()) return h;   // no URL, no app: a title alone creates nothing upstream
    h.push_back("HTTP-Referer: " + ref);
    if (const std::string t = header_safe(attribution.title); !t.empty()) h.push_back("X-OpenRouter-Title: " + t);
    if (const std::string c = header_safe(attribution.categories); !c.empty())
        h.push_back("X-OpenRouter-Categories: " + c);
    return h;
}

Json HttpLLM::body(const std::vector<core::Message>& messages, const Json& tools) const {
    Json b = core::json_obj();
    // The operator's fields go in first and every field the harness decides is set over
    // them: `extra` can name something this transport has no key for, never replace the
    // model, the messages or the stream we chose.
    if (extra.type == Json::Obj)
        for (const auto& [k, v] : extra.obj) core::obj_set(b, k, *v);
    core::obj_set(b, "model", core::json_str(model));

    // Exactly one system message, at index 0 — the shape a chat template may insist on (Qwen3.5
    // raises "System message must be at the beginning." otherwise, and llama-server turns that
    // into HTTP 500 for the whole turn). core::collapse_system_messages holds the rule and its
    // test; every emitter already writes the mid-conversation side channel as a hidden user item,
    // so this costs one scan over an already-valid list.
    std::vector<core::Message> wire = messages;

    // The wire's image window: only the last kImageWindow image-bearing
    // messages ship their payloads. Older ones keep their text and the image
    // NAMES (the digest sits in the tool result's text, written there by the
    // handler) — so an old picture reads as "there was an image here" rather
    // than silently vanishing, and a long transcript never ships one b64 blob
    // after another. The file on disk plus its digest is the recovery path:
    // the agent can re-view_image and get the same digest back.
    {
        constexpr int kImageWindow = 4;
        int kept = 0;
        for (auto it = wire.rbegin(); it != wire.rend(); ++it) {
            if (it->images.empty()) continue;
            if (kept >= kImageWindow) {
                it->images.clear();
                it->content += "\n(image payload dropped: outside the wire's last-4-image window)";
            } else {
                ++kept;
            }
        }
    }

    core::collapse_system_messages(wire);
    Json msgs = core::json_arr();
    for (const auto& m : wire) core::arr_push(msgs, wire_message(m));
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
    const auto t0 = std::chrono::steady_clock::now();
    double instructed_spent = 0.0;   // what this call has slept on the SERVER's instructions
    for (int attempt = 1; attempt <= sends; ++attempt) {
        // Same identity as a main call: a prewarm is a full prompt the provider bills and files
        // under an app, so leaving it anonymous would put real traffic in the "Unknown" bucket.
        const net::Response r = net::post(endpoint, payload, timeout_ms, stop, net::kMaxBody, attribution_headers());
        if (r.ok()) return true;
        // A cache fill that never landed is worth one more fill; a cut call, a refused
        // url and a verdict on the request are all final — the same no comes back.
        if (!transient(wire_failure(wire_result(r))) || attempt == sends) return false;
        // A rate limit is waited out on the ladder the operator named; a socket fact keeps the
        // millisecond scale, where a restarted server is back before the second send.
        const double left = wait_room(t0, retry_budget_ms, r.retry_after_s, retry_after_cap_ms,
                                     retry_after_total_ms, instructed_spent);
        const double wait = r.status == 429 ? wire_rate_wait_ms(attempt, r.retry_after_s, left)
                                           : wire_wait_ms(attempt, r.retry_after_s, 250.0, 2000.0, left);
        if (wait < 0.0) return false;   // the server asked for longer than this call may spend
        if (r.retry_after_s > 0) instructed_spent += wait;
        if (!wait_backoff(wait, stop)) return false;
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
    // What the send loop did to this call, for the operator who asked to see the wire: the pause
    // it sat through, and the status that made it sit. A rate limit that cleared is invisible
    // without these, and looks exactly like a slow endpoint.
    double waited_ms = 0.0, last_wait_ms = 0.0;
    double instructed_spent = 0.0;   // what this call has slept on the SERVER's instructions
    long first_status = 0;

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
        // Who this traffic belongs to, when the operator (or Ariadne's own default) named an
        // app: a provider that files by Referer shows "Unknown" for every un-attributed call.
        for (const std::string& h : attribution_headers()) hdrs = curl_slist_append(hdrs, h.c_str());
        struct SListGuard { struct curl_slist* h; ~SListGuard() { curl_slist_free_all(h); } } hguard{hdrs};

        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<curl_off_t>(payload.size()));
        curl_easy_setopt(c, CURLOPT_URL, endpoint.c_str());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, stream_sink);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
        curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, stream_header_cb);
        curl_easy_setopt(c, CURLOPT_HEADERDATA, &sink);
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
        // A write error with the terminator already seen is the reader stopping on its own,
        // even if Esc landed in the same instant: the answer arrived, so it is kept.
        const bool cancelled = (rc == CURLE_ABORTED_BY_CALLBACK && !sink.stalled)
                            || (rc == CURLE_WRITE_ERROR && flag && !sink.completed);

        WireFailure f = wire_failure(wire_result(rc, status, sink, cancelled));
        // A 200 whose body is neither a stream nor an answer still carries the failure: an
        // upstream provider that died before its first token comes back that way. Read as an
        // answer, the turn ends with an empty assistant message and the loop moves on as if the
        // model had said nothing -- a dead turn with no cause on screen.
        const bool bodiless = f == WireFailure::None && !sink.cs.saw_data() && !sink.cs.done() &&
                              sink.raw.find("\"error\"") != std::string::npos;
        if (bodiless) f = WireFailure::Transient;   // nothing was spoken: a second send can differ
        // The provider's own failure before its first token, told apart from a socket fact by the
        // response line: a provider always answered something (2xx), a cut socket never did.
        const bool upstream_hiccup = bodiless || (f == WireFailure::Transient && status >= 200 &&
                                                  status < 400 && !sink.spoke);
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
                st.status = status;
                st.first_status = first_status;
                st.waited_ms = waited_ms;
                st.last_wait_ms = last_wait_ms;
                st.retry_after_s = sink.retry_after_s;
                for (const auto& [k, v] : sink.headers) {
                    if (!st.headers.empty()) st.headers += " · ";
                    st.headers += k + ": " + v;
                }
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
        // Only a state that can differ is sent again, and only while nothing has been handed
        // out and the budget has time for the wait. A cut call, a verdict on the request, a
        // capped body, a stalled server and the last send all end here.
        if (!transient(f) || attempt == sends) throw std::runtime_error(with_sends(last, attempt));
        // The provider's own failure before its first token is worth one more send, not a
        // ladder's worth: a status said nothing about wanting a wait (200), and if the second
        // send comes back with the same sentence, the request is what it dislikes.
        if (upstream_hiccup && attempt >= 2) throw std::runtime_error(with_sends(last, attempt));
        // A PROVIDER-level 429 is a routing fact, not an instruction to this client. The number in
        // Retry-After is the endpoint's window for requests that must come BACK to that endpoint; the
        // next send may land on another one, so sitting through it is how a first prompt became three
        // minutes of "⟳ 429 · waiting 180s" against a provider that was simply saturated. Two quick
        // sends on the ladder, then the server's own sentence and the knob that decides who may route.
        const bool routed_refusal = status == 429 && last.find("Provider returned error") != std::string::npos;
        if (routed_refusal && attempt >= 2)
            throw std::runtime_error(with_sends(last, attempt) +
                                     " (an endpoint behind the route refused it: that is a routing fact, not a wait -- which "
                                     "providers may answer is the account's own allowed-provider setting, and a pinned or "
                                     "throttled one is the usual cause)");
        const long instruction = routed_refusal ? 0L : sink.retry_after_s;   // a refused route is not a wait
        const double left = wait_room(t0, retry_budget_ms, instruction, retry_after_cap_ms,
                                     retry_after_total_ms, instructed_spent);
        const double wait = status == 429 ? wire_rate_wait_ms(attempt, instruction, left)
                                         : wire_wait_ms(attempt, instruction, 250.0, 2000.0, left);
        if (wait < 0.0)
            throw std::runtime_error(budget_spent(last, attempt, instruction, retry_budget_ms,
                                                  retry_after_cap_ms, retry_after_total_ms, instructed_spent));
        if (first_status == 0) first_status = status;
        last_wait_ms = wait;
        waited_ms += wait;
        if (instruction > 0) instructed_spent += wait;
        // A pause of two minutes must not look like a dead program: say it before going quiet -- with
        // the reason the server gave, because "429" alone leaves the operator guessing whether it is a
        // rate limit, an exhausted key or a pin on a throttled provider -- and say it again when the
        // wait ends. The operator can still stop the turn while it sleeps.
        if (on_wait) on_wait(status, instruction, wait, last);
        if (!wait_backoff(wait, cancel)) throw std::runtime_error("interrupted");
        if (on_wait) on_wait(0, 0, 0.0, std::string());
    }

    throw std::runtime_error(with_sends(last, sends));   // unreachable: the final send throws above
}

}  // namespace agent
}  // namespace dd
