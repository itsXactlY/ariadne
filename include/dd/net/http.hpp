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
    // A streamed call whose callback asked for the end. The server answered; the caller
    // stopped reading. An error would call a satisfied reader a failure.
    bool ended_early = false;
    bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

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
