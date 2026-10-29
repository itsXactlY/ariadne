// src/serve/chat_api.cpp — impl of dd/serve/chat_api.hpp.

#include "dd/serve/chat_api.hpp"

#include <chrono>
#include <cstdio>
#include <stdexcept>

#include "dd/core/json.hpp"

namespace dd {
namespace serve {

using core::Json;
using core::json_dump;
using core::json_get;
using core::json_str;
using core::obj_set;

namespace {

std::string error_body(const std::string& message, const char* type) {
    Json e = core::json_obj();
    obj_set(e, "message", json_str(message));
    obj_set(e, "type", json_str(type));
    Json o = core::json_obj();
    obj_set(o, "error", e);
    return json_dump(o);
}

// The text of a message's content: a string, or the text parts of an array.
std::string content_text(const Json& content) {
    if (content.type == Json::Str) return content.str;
    std::string out;
    if (content.type == Json::Arr)
        for (const auto& part : content.arr) {
            if (!part || part->type != Json::Obj) continue;
            const Json* t = json_get(*part, "text");
            if (t && t->type == Json::Str) out += (out.empty() ? "" : "\n") + t->str;
        }
    return out;
}

long now_s() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

ChatApi::ChatApi(ApiInfo info, std::string api_key, TurnRunner& runner)
    : info_(std::move(info)), api_key_(std::move(api_key)), runner_(runner) {}

bool ChatApi::authorized(const net::Request& req) const {
    return api_key_.empty() || req.header("authorization") == "Bearer " + api_key_;
}

void ChatApi::handle(const net::Request& req, net::Conn& conn) {
    if (req.path == "/health") {
        conn.json(200, R"({"status":"ok","agent":"ariadne"})");
        return;
    }
    if (!authorized(req)) {
        conn.json(401, error_body("missing or wrong bearer token", "auth_error"));
        return;
    }
    if (req.path == "/health/detailed") {
        Json o = core::json_obj();
        obj_set(o, "status", json_str("ok"));
        obj_set(o, "agent", json_str("ariadne"));
        obj_set(o, "model", json_str(info_.upstream_model));
        obj_set(o, "home", json_str(info_.home));
        obj_set(o, "tools", core::json_num(info_.tools));
        obj_set(o, "busy", core::json_bool(busy()));
        conn.json(200, json_dump(o));
        return;
    }
    if (req.path == "/v1/models") {
        Json m = core::json_obj();
        obj_set(m, "id", json_str(info_.model_id));
        obj_set(m, "object", json_str("model"));
        obj_set(m, "owned_by", json_str("ariadne"));
        Json list = core::json_obj();
        obj_set(list, "object", json_str("list"));
        Json data = core::json_arr();
        core::arr_push(data, m);
        obj_set(list, "data", data);
        conn.json(200, json_dump(list));
        return;
    }
    if (req.path == "/v1/chat/completions") {
        if (req.method != "POST") {
            conn.json(405, error_body("POST only", "invalid_request_error"));
            return;
        }
        chat(req, conn);
        return;
    }
    conn.json(404, error_body("no route " + req.path, "invalid_request_error"));
}

std::string ChatApi::chunk(const std::string& delta_json, const char* finish) const {
    return R"({"id":")" + turn_id_ + R"(","object":"chat.completion.chunk","created":)" +
           std::to_string(created_) + R"(,"model":)" + json_dump(json_str(info_.model_id)) +
           R"(,"choices":[{"index":0,"delta":)" + delta_json + R"(,"finish_reason":)" +
           (finish ? json_dump(json_str(finish)) : std::string("null")) + "}]}";
}

void ChatApi::emit(const std::string& delta_json) {
    net::Conn* c = stream_.load(std::memory_order_acquire);
    if (!c) return;
    if (!c->sse_data(chunk(delta_json, nullptr))) runner_.interrupt();   // the client left
}

void ChatApi::on_content(const std::string& delta) {
    streamed_text_ += delta;
    Json d = core::json_obj();
    obj_set(d, "content", json_str(delta));
    emit(json_dump(d));
}

void ChatApi::on_reasoning(const std::string& delta) {
    Json d = core::json_obj();
    obj_set(d, "reasoning_content", json_str(delta));
    emit(json_dump(d));
}

void ChatApi::on_message(const std::string& content) {
    if (content.empty()) return;
    on_content(content);
}

void ChatApi::on_tool_call(const std::string& id, const std::string& name, const std::string& arguments) {
    Json t = core::json_obj();
    obj_set(t, "id", json_str(id));
    obj_set(t, "name", json_str(name));
    obj_set(t, "arguments", json_str(arguments));
    Json d = core::json_obj();
    obj_set(d, "ariadne_tool_call", t);   // an extension: a plain OpenAI client ignores unknown delta keys
    emit(json_dump(d));
}

void ChatApi::on_tool_result(const std::string& id, const std::string& content) {
    Json t = core::json_obj();
    obj_set(t, "id", json_str(id));
    const bool cut = content.size() > kToolResultMax;
    obj_set(t, "content", json_str(cut ? content.substr(0, kToolResultMax) : content));
    obj_set(t, "truncated", core::json_bool(cut));
    Json d = core::json_obj();
    obj_set(d, "ariadne_tool_result", t);
    emit(json_dump(d));
}

void ChatApi::chat(const net::Request& req, net::Conn& conn) {
    Json body;
    try {
        body = core::json_parse(req.body);
    } catch (const std::exception& ex) {
        conn.json(400, error_body(std::string("body is not JSON: ") + ex.what(), "invalid_request_error"));
        return;
    }
    const Json* messages = body.type == Json::Obj ? json_get(body, "messages") : nullptr;
    if (!messages || messages->type != Json::Arr) {
        conn.json(400, error_body("messages must be an array", "invalid_request_error"));
        return;
    }
    std::string question;
    for (auto it = messages->arr.rbegin(); it != messages->arr.rend(); ++it) {
        if (!*it || (*it)->type != Json::Obj) continue;
        const Json* role = json_get(**it, "role");
        const Json* content = json_get(**it, "content");
        if (role && role->type == Json::Str && role->str == "user" && content) {
            question = content_text(*content);
            break;
        }
    }
    if (question.empty()) {
        conn.json(400, error_body("no user message with text", "invalid_request_error"));
        return;
    }
    const Json* stream_flag = json_get(body, "stream");
    const bool stream = stream_flag && stream_flag->type == Json::Bool && stream_flag->b;

    bool expected = false;
    if (!busy_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        conn.json(409, error_body("ariadne is in a turn; one turn at a time", "busy"));
        return;
    }
    struct Release {
        std::atomic<bool>& b;
        std::atomic<net::Conn*>& s;
        ~Release() {
            s.store(nullptr, std::memory_order_release);
            b.store(false, std::memory_order_release);
        }
    } release{busy_, stream_};

    created_ = now_s();
    turn_id_ = "chatcmpl-ariadne-" + std::to_string(created_) + "-" + std::to_string(++turns_);
    streamed_text_.clear();
    if (stream) {
        if (!conn.sse_begin()) return;
        stream_.store(&conn, std::memory_order_release);
        emit(R"({"role":"assistant"})");
    }

    std::string answer, failure;
    try {
        answer = runner_.run(question);
    } catch (const std::exception& ex) {
        failure = ex.what();
    }

    if (stream) {
        if (!failure.empty()) {
            Json d = core::json_obj();
            obj_set(d, "content", json_str("\n[ariadne: " + failure + "]"));
            emit(json_dump(d));
        }
        if (net::Conn* c = stream_.load(std::memory_order_acquire)) {
            c->sse_data(chunk("{}", failure.empty() ? "stop" : "error"));
            c->sse_data("[DONE]");
        }
        return;
    }
    if (!failure.empty()) {
        conn.json(500, error_body("the turn failed: " + failure, "agent_error"));
        return;
    }
    Json msg = core::json_obj();
    obj_set(msg, "role", json_str("assistant"));
    obj_set(msg, "content", json_str(answer.empty() ? streamed_text_ : answer));
    Json choice = core::json_obj();
    obj_set(choice, "index", core::json_num(0));
    obj_set(choice, "message", msg);
    obj_set(choice, "finish_reason", json_str("stop"));
    Json o = core::json_obj();
    obj_set(o, "id", json_str(turn_id_));
    obj_set(o, "object", json_str("chat.completion"));
    obj_set(o, "created", core::json_num(static_cast<double>(created_)));
    obj_set(o, "model", json_str(info_.model_id));
    Json choices = core::json_arr();
    core::arr_push(choices, choice);
    obj_set(o, "choices", choices);
    conn.json(200, json_dump(o));
}

}  // namespace serve
}  // namespace dd
