#pragma once
// dd/net/http.hpp — one-shot HTTP over libcurl for probes and side requests
// (server /props /slots /health, slot erase, prewarm). A set cancel flag
// aborts a request in flight. curl_global_init must run before threads use it.

#include <atomic>
#include <string>

namespace dd {
namespace net {

struct Response {
    long status = 0;
    std::string body;
    std::string error;   // transport failure, empty when the server answered
    bool cancelled = false;
    bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

Response get(const std::string& url, long timeout_ms);
Response post(const std::string& url, const std::string& json_body, long timeout_ms,
              const std::atomic<bool>* cancel = nullptr);

}  // namespace net
}  // namespace dd
