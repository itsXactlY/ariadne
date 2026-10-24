// src/tools/mcp_client.cpp — Streamable-HTTP MCP client over libcurl.
#include "dd/tools/mcp.hpp"
#include "dd/tools/url_safety.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "dd/core/json.hpp"

namespace dd {
namespace tools {

namespace {

using core::Json;
using core::JsonSP;

constexpr const char* kProtocolVersion = "2025-06-18";

std::atomic<long long> g_next_id{1};

JsonSP jsp(Json v) { return std::make_shared<Json>(std::move(v)); }

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

struct Raw {
    long status = 0;
    std::string body, ctype, session, error;
};

struct Sink {
    std::string* body;
    size_t max;
    bool over = false;
};

// libcurl 8.16+ write callback: (buffer, capacity, actual_bytes, outstream).
std::size_t body_sink(char* buffer, std::size_t size, std::size_t nitems, void* ud) {
    auto* s = static_cast<Sink*>(ud);
    const std::size_t n = size * nitems;
    if (s->body->size() + n > s->max) {
        s->over = true;
        return 0;
    }
    s->body->append(buffer, n);
    return n;
}

// "Mcp-Session-Id: abc\r\n"
std::size_t header_sink(char* buffer, std::size_t size, std::size_t nitems, void* ud) {
    const std::size_t n = size * nitems;
    const std::string line(buffer, n);
    const size_t colon = line.find(':');
    if (colon != std::string::npos && lower(line.substr(0, colon)) == "mcp-session-id")
        *static_cast<std::string*>(ud) = trim(line.substr(colon + 1));
    return n;
}

Raw http_post(const std::string& url, long long timeout_ms, size_t max_bytes, const std::vector<std::string>& hdr,
              const std::string& payload) {
    Raw raw;
    if (!dd::tools::is_safe_url(url)) {
        raw.error = "url rejected: " + url;
        return raw;
    }
    CURL* c = curl_easy_init();
    if (!c) throw std::runtime_error("mcp: curl handle allocation failed");
    struct CurlGuard { CURL* c; ~CurlGuard() { curl_easy_cleanup(c); } } guard{c};

    struct curl_slist* hdrs = nullptr;
    for (const auto& h : hdr) hdrs = curl_slist_append(hdrs, h.c_str());
    struct SListGuard { struct curl_slist* h; ~SListGuard() { curl_slist_free_all(h); } } hguard{hdrs};

    Sink out{&raw.body, max_bytes};
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (curl_off_t)payload.size());
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, body_sink);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, header_sink);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &raw.session);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");

    const CURLcode rc = curl_easy_perform(c);
    if (rc == CURLE_WRITE_ERROR && out.over) {
        raw.error = "response over " + std::to_string(max_bytes) + " bytes";
    } else if (rc != CURLE_OK) {
        raw.error = curl_easy_strerror(rc);
    }
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &raw.status);
    const char* ct = nullptr;
    if (curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &ct) == CURLE_OK && ct) raw.ctype = lower(ct);
    return raw;
}

void flatten(Json v, std::vector<Json>& out) {
    if (v.type == Json::Arr) {
        for (const auto& e : v.arr)
            if (e && e->type == Json::Obj) out.push_back(core::json_deep_copy(*e));
    } else if (v.type == Json::Obj) {
        out.push_back(std::move(v));
    }
}

// SSE per the spec: events end at a blank line, `data:` lines of one event join with "\n", one space after the colon
// is dropped, `:` lines are comments, CR/CRLF/LF all end a line.
void sse_events(const std::string& body, std::vector<Json>& out) {
    std::string data;
    bool has = false;
    auto flush = [&] {
        if (has && !data.empty()) {
            try { flatten(core::json_parse(data), out); } catch (...) {}
        }
        data.clear();
        has = false;
    };
    size_t i = 0;
    const size_t n = body.size();
    while (i < n) {
        size_t e = i;
        while (e < n && body[e] != '\n' && body[e] != '\r') ++e;
        const std::string line = body.substr(i, e - i);
        if (e < n && body[e] == '\r' && e + 1 < n && body[e + 1] == '\n') ++e;
        i = e + 1;
        if (line.empty()) {
            flush();
            continue;
        }
        if (line[0] == ':') continue;
        const size_t colon = line.find(':');
        const std::string field = colon == std::string::npos ? line : line.substr(0, colon);
        std::string value = colon == std::string::npos ? std::string() : line.substr(colon + 1);
        if (!value.empty() && value[0] == ' ') value.erase(0, 1);
        if (field == "data") {
            if (has) data += '\n';
            data += value;
            has = true;
        }
    }
    flush();
}

void json_body(const std::string& body, std::vector<Json>& out) {
    try { flatten(core::json_parse(body), out); } catch (...) {}
}

std::vector<Json> objects_of(const Raw& raw) {
    std::vector<Json> out;
    const size_t first = raw.body.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return out;
    const bool json_like = raw.body[first] == '{' || raw.body[first] == '[';
    if (raw.ctype.find("event-stream") != std::string::npos || !json_like) {
        sse_events(raw.body, out);
        if (out.empty() && json_like) json_body(raw.body, out);
    } else {
        json_body(raw.body, out);
    }
    return out;
}

bool id_is(const Json& obj, long long id) {
    const Json* v = core::json_get(obj, "id");
    if (!v) return false;
    if (v->type == Json::Num) return static_cast<long long>(v->num) == id;
    if (v->type == Json::Str) return v->str == std::to_string(id);
    return false;
}

bool answers(const Json& obj) { return core::json_get(obj, "result") || core::json_get(obj, "error"); }

// the object that answers request `id`; one with no id at all stands in when nothing matches
const Json* pick(const std::vector<Json>& objs, long long id) {
    for (const auto& o : objs)
        if (answers(o) && id_is(o, id)) return &o;
    for (const auto& o : objs) {
        const Json* v = core::json_get(o, "id");
        if (answers(o) && (!v || v->type == Json::Null)) return &o;
    }
    return nullptr;
}

std::string snippet(const std::string& s) {
    std::string o;
    for (char c : s.substr(0, 200)) o.push_back(c == '\n' || c == '\r' ? ' ' : c);
    return o;
}

std::string str_of(const Json* v) { return v && v->type == Json::Str ? v->str : std::string(); }

}  // namespace

std::string MCPClient::expand_env(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '$' && i + 1 < s.size() && s[i + 1] == '{') {
            const size_t e = s.find('}', i + 2);
            if (e != std::string::npos && e > i + 2) {
                const std::string name = s.substr(i + 2, e - i - 2);
                const bool ok = std::all_of(name.begin(), name.end(), [](char c) {
                    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
                });
                if (ok) {
                    if (const char* v = std::getenv(name.c_str())) out += v;
                    i = e + 1;
                    continue;
                }
            }
        }
        out.push_back(s[i++]);
    }
    return out;
}

MCPClient::Outcome MCPClient::exchange(const std::string& method, const Json& params, bool notification) const {
    const long long id = notification ? 0 : g_next_id.fetch_add(1, std::memory_order_relaxed);
    Json req = core::json_obj({{"jsonrpc", jsp(core::json_str("2.0"))}});
    if (!notification) core::obj_set(req, "id", core::json_num(static_cast<double>(id)));
    core::obj_set(req, "method", core::json_str(method));
    if (!(notification && params.type == Json::Null)) core::obj_set(req, "params", core::json_deep_copy(params));
    const std::string payload = core::json_dump(req);

    std::vector<std::string> hdr = {"Content-Type: application/json", "Accept: application/json, text/event-stream"};
    if (const auto s = session_.read(); s && !s->empty()) hdr.push_back("Mcp-Session-Id: " + *s);
    if (initialized_.load(std::memory_order_acquire)) hdr.push_back(std::string("MCP-Protocol-Version: ") + kProtocolVersion);
    for (const auto& [name, value] : headers) {
        const std::string v = expand_env(value);
        if (name.empty() || name.find_first_of(":\r\n ") != std::string::npos || v.find_first_of("\r\n") != std::string::npos)
            throw std::runtime_error("mcp: bad header '" + name + "'");
        // an unset env expands to nothing: libcurl reads `Name:` with an empty
        // value as "remove this header" and never sends it, so drop it here and
        // keep the client's own default for that name
        if (v.empty()) continue;
        const std::string ln = lower(name);
        hdr.erase(std::remove_if(hdr.begin(), hdr.end(),
                                 [&](const std::string& h) { return lower(h.substr(0, h.find(':'))) == ln; }),
                  hdr.end());
        hdr.push_back(name + ": " + v);
    }

    const Raw raw = http_post(url, timeout_ms, max_bytes, hdr, payload);
    if (!raw.error.empty()) throw std::runtime_error("mcp: " + raw.error);

    Outcome out;
    if (notification) return out;
    const std::vector<Json> objs = objects_of(raw);
    const Json* sel = pick(objs, id);
    if (!sel) {
        if (raw.status >= 300) throw std::runtime_error("mcp: HTTP " + std::to_string(raw.status) + ": " + snippet(raw.body));
        return out;
    }
    out.got = true;
    if (const Json* e = core::json_get(*sel, "error"); e && e->type == Json::Obj) {
        out.failed = true;
        if (const Json* c = core::json_get(*e, "code"); c && c->type == Json::Num) out.code = static_cast<long long>(c->num);
        out.message = str_of(core::json_get(*e, "message"));
        if (out.message.empty()) out.message = "error";
        return out;
    }
    if (const Json* r = core::json_get(*sel, "result")) out.result = core::json_deep_copy(*r);
    if (method == "initialize") {
        if (!raw.session.empty()) session_.publish(std::make_unique<const std::string>(raw.session));
        initialized_.store(true, std::memory_order_release);
    }
    return out;
}

Json MCPClient::rpc(const std::string& method, const Json& params) const {
    Outcome o = exchange(method, params, false);
    if (o.failed) throw std::runtime_error("mcp: " + std::to_string(o.code) + " " + o.message);
    return o.got ? std::move(o.result) : Json{};
}

std::string MCPClient::instructions() {
    Json params = core::json_obj(
        {{"protocolVersion", jsp(core::json_str(kProtocolVersion))},
         {"capabilities", jsp(core::json_obj())},
         {"clientInfo", jsp(core::json_obj({{"name", jsp(core::json_str("daedalus-cpp"))}, {"version", jsp(core::json_str("0.1"))}}))}});
    Json res = rpc("initialize", params);
    try { exchange("notifications/initialized", Json{}, true); } catch (...) {}   // best effort: a stateless server may not care
    if (res.type != Json::Obj) return "";
    const Json* ins = core::json_get(res, "instructions");
    return (ins && ins->type == Json::Str) ? ins->str : std::string();
}

Json MCPClient::list_tools() {
    Json all = core::json_arr();
    std::string cursor;
    for (int page = 0; page < 20; ++page) {
        Json params = core::json_obj();
        if (!cursor.empty()) core::obj_set(params, "cursor", core::json_str(cursor));
        Json res = rpc("tools/list", params);
        const Json* tools = res.type == Json::Obj ? core::json_get(res, "tools") : nullptr;
        if (!tools || tools->type != Json::Arr) return page == 0 ? Json{} : all;
        for (const auto& t : tools->arr)
            if (t) core::arr_push(all, core::json_deep_copy(*t));
        const Json* next = core::json_get(res, "nextCursor");
        if (!next || next->type != Json::Str || next->str.empty()) break;
        cursor = next->str;
    }
    return all;
}

McpResult MCPClient::call_ex(const std::string& name, const Json& args) {
    McpResult r;
    try {
        Json params = core::json_obj({{"name", jsp(core::json_str(name))}, {"arguments", jsp(core::json_deep_copy(args))}});
        Outcome o = exchange("tools/call", params, false);
        if (!o.got) {
            r.error = "mcp: no JSON-RPC answer in the response";
            return r;
        }
        if (o.failed) {
            r.error = "mcp: " + std::to_string(o.code) + " " + o.message;
            return r;
        }
        r.ok = true;
        const Json& res = o.result;
        if (res.type == Json::Str) {
            r.text = res.str;
        } else if (res.type == Json::Obj) {
            if (const Json* content = core::json_get(res, "content"); content && content->type == Json::Arr)
                for (const auto& part : content->arr) {
                    if (!part || part->type != Json::Obj) continue;
                    std::string t;
                    if (str_of(core::json_get(*part, "type")) == "resource") {
                        if (const Json* rs = core::json_get(*part, "resource")) t = str_of(core::json_get(*rs, "text"));
                    } else {
                        t = str_of(core::json_get(*part, "text"));
                    }
                    if (t.empty()) continue;
                    if (!r.text.empty()) r.text += "\n";
                    r.text += t;
                }
            if (r.text.empty())
                if (const Json* sc = core::json_get(res, "structuredContent")) r.text = core::json_dump(*sc);
            if (const Json* e = core::json_get(res, "isError"); e && e->type == Json::Bool && e->b) {
                r.is_error = true;
                r.error = r.text.empty() ? "the tool reported an error" : r.text;
            }
        }
    } catch (const std::exception& e) {
        r.ok = false;
        r.error = e.what();
    }
    return r;
}

std::string MCPClient::call(const std::string& name, const Json& args) {
    McpResult r = call_ex(name, args);
    if (r.ok && !r.is_error) return r.text;
    return core::json_dump(core::json_obj({{"error", jsp(core::json_str(r.error))}, {"isError", jsp(core::json_bool(true))}}));
}

}  // namespace tools
}  // namespace dd
