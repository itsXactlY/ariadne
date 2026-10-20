// src/net/http.cpp — impl of dd/net/http.hpp.
#include "dd/net/http.hpp"

#include <curl/curl.h>

namespace dd {
namespace net {

namespace {

// libcurl 8.16+ write callback: (buffer, capacity, actual_bytes, outstream).
std::size_t sink(char* p, std::size_t, std::size_t n, void* ud) {
    static_cast<std::string*>(ud)->append(p, n);
    return n;
}

int abort_if(void* ud, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto* flag = static_cast<const std::atomic<bool>*>(ud);
    return flag && flag->load(std::memory_order_relaxed) ? 1 : 0;
}

Response perform(const std::string& url, const std::string* body, long timeout_ms, const std::atomic<bool>* cancel) {
    Response r;
    CURL* c = curl_easy_init();
    if (!c) {
        r.error = "curl: no handle";
        return r;
    }
    struct curl_slist* hdrs = nullptr;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, std::min(timeout_ms, 3000L));
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
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
    if (rc == CURLE_ABORTED_BY_CALLBACK) {
        r.cancelled = true;
        r.error = "cancelled";
    } else if (rc != CURLE_OK) {
        r.error = curl_easy_strerror(rc);
    }
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    return r;
}

}  // namespace

Response get(const std::string& url, long timeout_ms) { return perform(url, nullptr, timeout_ms, nullptr); }

Response post(const std::string& url, const std::string& json_body, long timeout_ms, const std::atomic<bool>* cancel) {
    return perform(url, &json_body, timeout_ms, cancel);
}

}  // namespace net
}  // namespace dd
