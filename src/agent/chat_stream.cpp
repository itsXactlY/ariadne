// src/agent/chat_stream.cpp — impl of dd/agent/chat_stream.hpp.
#include "dd/agent/chat_stream.hpp"

#include <algorithm>

namespace dd {
namespace agent {

using core::Json;

void ChatStream::feed(const char* p, size_t n) {
    if (head_.size() < 2048) head_.append(p, std::min(n, 2048 - head_.size()));
    buf_.append(p, n);
    size_t start = 0, nl;
    while ((nl = buf_.find('\n', start)) != std::string::npos) {
        std::string l = buf_.substr(start, nl - start);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        line(l);
        start = nl + 1;
    }
    buf_.erase(0, start);
}

void ChatStream::finish() {
    if (!buf_.empty()) line(buf_);
    buf_.clear();
}

void ChatStream::line(const std::string& l) {
    // SSE: "data: <json>" carries a chunk; ":" lines are comments (keep-alive)
    if (l.compare(0, 5, "data:") != 0) return;
    std::string payload = l.substr(5);
    if (!payload.empty() && payload[0] == ' ') payload.erase(0, 1);
    if (payload == "[DONE]") return;
    Json j;
    try {
        j = core::json_parse(payload);
    } catch (...) {
        return;
    }
    ++chunks_;
    if (const Json* e = core::json_get(j, "error")) {
        const Json* m = e->type == Json::Obj ? core::json_get(*e, "message") : nullptr;
        error_ = m && m->type == Json::Str ? m->str : core::json_dump(*e);
    }
    if (const Json* u = core::json_get(j, "usage"); u && u->type == Json::Obj) usage_ = *u;
    if (const Json* t = core::json_get(j, "timings"); t && t->type == Json::Obj) timings_ = *t;
    const Json* choices = core::json_get(j, "choices");
    if (!choices || choices->type != Json::Arr || choices->arr.empty() || !choices->arr[0]) return;
    const Json& ch = *choices->arr[0];
    if (const Json* fr = core::json_get(ch, "finish_reason"); fr && fr->type == Json::Str) finish_ = fr->str;
    const Json* d = core::json_get(ch, "delta");
    if (!d || d->type != Json::Obj) return;
    for (const char* key : {"reasoning_content", "reasoning"})
        if (const Json* r = core::json_get(*d, key); r && r->type == Json::Str && !r->str.empty()) {
            reasoning_ += r->str;
            if (on_reasoning) on_reasoning(r->str);
            break;
        }
    if (const Json* c = core::json_get(*d, "content"); c && c->type == Json::Str && !c->str.empty()) {
        content_ += c->str;
        if (on_content) on_content(c->str);
    }
    if (const Json* tcs = core::json_get(*d, "tool_calls"); tcs && tcs->type == Json::Arr) {
        for (const auto& sp : tcs->arr) {
            if (!sp || sp->type != Json::Obj) continue;
            const Json* ix = core::json_get(*sp, "index");
            core::ToolCall& tc = calls_[ix && ix->type == Json::Num ? static_cast<long>(ix->num) : 0];
            if (const Json* id = core::json_get(*sp, "id"); id && id->type == Json::Str && !id->str.empty()) tc.id = id->str;
            if (const Json* fn = core::json_get(*sp, "function"); fn && fn->type == Json::Obj) {
                if (const Json* nm = core::json_get(*fn, "name"); nm && nm->type == Json::Str) tc.name += nm->str;
                if (const Json* a = core::json_get(*fn, "arguments"); a && a->type == Json::Str) tc.arguments += a->str;
            }
        }
    }
}

core::Message ChatStream::message() const {
    core::Message m;
    m.role = "assistant";
    m.content = content_;
    m.finish_reason = finish_;
    for (const auto& [ix, tc] : calls_) {
        core::ToolCall c = tc;
        if (c.id.empty()) c.id = "call_" + std::to_string(ix);
        if (c.arguments.empty()) c.arguments = "{}";
        m.tool_calls.push_back(std::move(c));
    }
    return m;
}

}  // namespace agent
}  // namespace dd
