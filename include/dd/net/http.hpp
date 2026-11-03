#pragma once
// dd/net/http.hpp — one-shot HTTP over libcurl for probes and side requests
// (server /props /slots /health, slot erase, prewarm). A set cancel flag
// aborts a request in flight. curl_global_init must run before threads use it.
// http and https only, no redirects; a body past max_bytes aborts the transfer.

#include <atomic>
#include <cstddef>
#include <string>
#include <functional>
#include <string_view>

namespace dd {
namespace net {

constexpr size_t kMaxBody = 8u << 20;

struct Response {
    long status = 0;
    std::string body;
    std::string error;   // transport failure, empty when the server answered
    bool cancelled = false;
    // The server's own Retry-After, in whole seconds (0 = it named none). A 429 or a 503
    // that says how long to wait is not a verdict on the request, it is an instruction;
    // the retry policy honours it rather than guessing a backoff that comes back too early.
    long retry_after_s = 0;
    // A streamed call whose callback asked for the end. The server answered; the caller
    // stopped reading. An error would call a satisfied reader a failure.
    bool ended_early = false;
    bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

// `Retry-After: 12` -> 12; a header line that is not that header, or a HTTP-date form this
// client cannot schedule against, -> 0. One parser, so the one-shot and the streamed paths
// cannot disagree about what a server asked for.
long retry_after_seconds(const std::string& header_line);

Response get(const std::string& url, long timeout_ms, size_t max_bytes = kMaxBody);
Response post(const std::string& url, const std::string& json_body, long timeout_ms,
              const std::atomic<bool>* cancel = nullptr, size_t max_bytes = kMaxBody);

// One POST whose bytes are handed out as they arrive, never buffered whole: an event stream
// (llama-server's SSE) is a body with no end a caller could wait for. `on_chunk` returning
// false ends the transfer, which is a satisfied reader, not a failure. `timeout_ms` is a
// STALL ceiling, not a wall ceiling: a stream that keeps producing may run for any length,
// and a stream that produces nothing for that long is a dead server.
Response post_stream(const std::string& url, const std::string& json_body, long timeout_ms,
                    const std::atomic<bool>* cancel,
                    const std::function<bool(std::string_view)>& on_chunk);

}  // namespace net
}  // namespace dd
