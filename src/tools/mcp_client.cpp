// src/tools/mcp_client.cpp — Streamable-HTTP MCP client over libcurl.
#include "dd/tools/mcp.hpp"
#include "dd/core/json.hpp"

#include <curl/curl.h>
#include <sstream>
#include <string>
#include <vector>

namespace dd {
namespace tools {

// libcurl 8.16+ write callback: (buffer, capacity, actual_bytes, outstream).
static std::size_t mcp_sink(char* buffer, std::size_t size, std::size_t nitems, void* ud) {
    (void)size;
    auto* s = static_cast<std::string*>(ud);
    s->append(buffer, nitems);
    return nitems;
}

// Pull every JSON-RPC object out of a response body. The body is SSE
// (`event: message` / `data: {…}` lines) or, for tolerant servers, a single
// bare JSON object. Returns each decodable object in order.
static std::vector<dd::core::Json> sse_objects(const std::string& body) {
    std::vector<dd::core::Json> out;
    auto feed = [&](const std::string& line) {
        if (line.rfind("data:", 0) == 0) {
            std::string payload = line.substr(5);
            while (!payload.empty() && (payload.front() == ' ' || payload.front() == '\t'))
                payload.erase(payload.begin());
            if (!payload.empty()) {
                try { out.push_back(dd::core::json_parse(payload)); } catch (...) {}
            }
        }
    };
    if (body.find('\n') == std::string::npos) {
        feed(body);  // bare single object
    } else {
        std::istringstream ss(body);
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            feed(line);
        }
    }
    return out;
}

dd::core::Json MCPClient::rpc(const std::string& method,
                              const dd::core::Json& params) const {

    dd::core::Json req = dd::core::json_obj(
        {{"jsonrpc", std::make_shared<dd::core::Json>(dd::core::json_str("2.0"))},
         {"id", std::make_shared<dd::core::Json>(dd::core::json_num(1))},
         {"method", std::make_shared<dd::core::Json>(dd::core::json_str(method))},
         {"params", std::make_shared<dd::core::Json>(dd::core::json_deep_copy(params))}});
    std::string payload = dd::core::json_dump(req);

    CURL* c = curl_easy_init();
    if (!c) throw std::runtime_error("mcp: curl handle allocation failed");
    struct CurlGuard { CURL* c; ~CurlGuard() { curl_easy_cleanup(c); } } guard{c};

    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Accept: application/json, text/event-stream");
    struct SListGuard { struct curl_slist* h; ~SListGuard() { curl_slist_free_all(h); } } hguard{hdrs};

    std::string out;
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (curl_off_t)payload.size());
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, mcp_sink);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, (curl_off_t)timeout_ms);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);

    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK)
        throw std::runtime_error(std::string("mcp: ") + curl_easy_strerror(rc));

    for (const dd::core::Json& obj : sse_objects(out)) {
        const dd::core::Json* res = dd::core::json_get(obj, "result");
        if (res) return dd::core::json_deep_copy(*res);
    }
    return dd::core::Json{};
}

std::string MCPClient::instructions() {
    dd::core::Json res = rpc("initialize", dd::core::json_obj());
    if (res.type != dd::core::Json::Obj) return "";
    const dd::core::Json* ins = dd::core::json_get(res, "instructions");
    return (ins && ins->type == dd::core::Json::Str) ? ins->str : std::string();
}

dd::core::Json MCPClient::list_tools() {
    dd::core::Json res = rpc("tools/list", dd::core::json_obj());
    if (res.type != dd::core::Json::Obj) return dd::core::Json{};
    const dd::core::Json* tools = dd::core::json_get(res, "tools");
    return tools ? dd::core::json_deep_copy(*tools) : dd::core::Json{};
}

std::string MCPClient::call(const std::string& name, const dd::core::Json& args) {
    dd::core::Json params = dd::core::json_obj(
        {{"name", std::make_shared<dd::core::Json>(dd::core::json_str(name))},
         {"arguments", std::make_shared<dd::core::Json>(dd::core::json_deep_copy(args))}});
    dd::core::Json res = rpc("tools/call", params);
    if (res.type != dd::core::Json::Obj) return "";
    const dd::core::Json* content = dd::core::json_get(res, "content");
    if (!content || content->type != dd::core::Json::Arr) return "";
    std::string text;
    for (const auto& sp : content->arr) {
        if (!sp || sp->type != dd::core::Json::Obj) continue;
        const dd::core::Json* t = dd::core::json_get(*sp, "text");
        if (t && t->type == dd::core::Json::Str) {
            if (!text.empty()) text += "\n";
            text += t->str;
        }
    }
    return text;
}

}  // namespace tools
}  // namespace dd
