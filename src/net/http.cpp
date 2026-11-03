// src/net/http.cpp — impl of dd/net/http.hpp.
#include "dd/net/http.hpp"
#include "dd/tools/url_safety.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>

namespace dd {
namespace net {

namespace {

struct Sink {
    std::string* out;
    size_t max;
    bool over = false;
};

// libcurl 8.16+ write callback: (buffer, capacity, actual_bytes, outstream).
std::size_t sink(char* p, std::size_t, std::size_t n, void* ud) {
    auto* s = static_cast<Sink*>(ud);
    if (s->out->size() + n > s->max) {
        s->over = true;
        return 0;
    }
    s->out->append(p, n);
    return n;
}

int abort_if(void* ud, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto* flag = static_cast<const std::atomic<bool>*>(ud);
    return flag && flag->load(std::memory_order_relaxed) ? 1 : 0;
}

// Every header line the server sends, once each: the only place a Retry-After can be read.
std::size_t header_cb(char* p, std::size_t, std::size_t n, void* ud) {
    auto* ra = static_cast<long*>(ud);
    const long secs = retry_after_seconds(std::string(p, n));
    if (secs > 0) *ra = secs;
    return n;
}

// A stream's sink: bytes go out as they arrive. 0 stops the transfer, which libcurl reports
// as CURLE_WRITE_ERROR — for a reader that saw enough, that is the end, not a failure.
struct StreamSink {
    const std::function<bool(std::string_view)>& on;
    bool stop = false;
};

std::size_t stream_sink(char* p, std::size_t, std::size_t n, void* ud) {
    auto* s = static_cast<StreamSink*>(ud);
    if (s->stop) return 0;
    if (!s->on(std::string_view(p, n))) {
        s->stop = true;
        return 0;
    }
    return n;
}

Response perform(const std::string& url, const std::string* body, long timeout_ms,
                 const std::atomic<bool>* cancel, size_t max_bytes, StreamSink* stream = nullptr) {
    Response r;
    if (!dd::tools::is_safe_url(url)) {
        r.error = "url rejected: " + url;
        return r;
    }
    CURL* c = curl_easy_init();
    if (!c) {
        r.error = "curl: no handle";
        return r;
    }
    Sink out{&r.body, max_bytes};
    long retry_after = 0;
    struct curl_slist* hdrs = nullptr;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &retry_after);
    if (stream) {
        // A stream is never buffered and never bounded by a wall clock: the transfer is
        // abandoned only when it produces nothing for `timeout_ms`, which is a dead server.
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, stream_sink);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, stream);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, std::max(1L, timeout_ms / 1000));
    } else {
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
        curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, timeout_ms);
    }
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, std::min(timeout_ms, 3000L));
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    if (body) {
        hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body->c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<curl_off_t>(body->size()));
    }
    if (cancel) {
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, abort_if);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, cancel);
    }
    const CURLcode rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    r.retry_after_s = retry_after;
    if (rc == CURLE_ABORTED_BY_CALLBACK) {
        r.cancelled = true;
        r.error = "cancelled";
    } else if (rc == CURLE_WRITE_ERROR && stream && stream->stop) {
        // The reader asked for the end mid-stream. The server answered fine; nothing is wrong.
        r.ended_early = true;
    } else if (rc == CURLE_WRITE_ERROR && out.over) {
        r.error = "response over " + std::to_string(max_bytes) + " bytes";
    } else if (rc != CURLE_OK) {
        r.error = curl_easy_strerror(rc);
    }
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    return r;
}

}  // namespace

long retry_after_seconds(const std::string& header_line) {
    static const std::string key = "retry-after:";
    if (header_line.size() <= key.size()) return 0;
    for (std::size_t i = 0; i < key.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(header_line[i])) != key[i]) return 0;
    const char* v = header_line.c_str() + key.size();
    while (*v == ' ' || *v == '\t') ++v;
    long secs = 0;
    bool digits = false;
    while (*v >= '0' && *v <= '9') {
        secs = secs * 10 + (*v - '0');
        digits = true;
        if (secs > 86400) return 86400;   // a day is already longer than any budget: clamp instead of overflowing
        ++v;
    }
    // The HTTP-date form (`Retry-After: Wed, 21 Oct 2015 07:28:00 GMT`) names an instant,
    // not a duration. This client has no wall clock the server agreed to, so it reads 0 and
    // falls back to its own backoff rather than inventing a wait.
    return digits ? secs : 0;
}

Response get(const std::string& url, long timeout_ms, size_t max_bytes) {
    return perform(url, nullptr, timeout_ms, nullptr, max_bytes);
}

Response post(const std::string& url, const std::string& json_body, long timeout_ms, const std::atomic<bool>* cancel,
              size_t max_bytes) {
    return perform(url, &json_body, timeout_ms, cancel, max_bytes);
}

Response post_stream(const std::string& url, const std::string& json_body, long timeout_ms,
                     const std::atomic<bool>* cancel, const std::function<bool(std::string_view)>& on_chunk) {
    StreamSink stream{on_chunk};
    // max_bytes 0: nothing is buffered, so no body ceiling applies.
    Response r = perform(url, &json_body, timeout_ms, cancel, 0, &stream);
    if (stream.stop && r.error.empty()) r.ended_early = true;
    return r;
}

}  // namespace net
}  // namespace dd
