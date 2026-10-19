// src/core/message.cpp — wire (de)serialization for messages.
#include "dd/core/message.hpp"
#include "dd/core/json.hpp"

namespace dd {
namespace core {

Json message_to_json(const Message& m) {
    Json o = json_obj();
    obj_set(o, "role", json_str(m.role));
    obj_set(o, "content", json_str(m.content));
    if (!m.tool_calls.empty()) {
        Json arr = json_arr();
        for (const auto& tc : m.tool_calls) {
            Json fn = json_obj();
            obj_set(fn, "name", json_str(tc.name));
            obj_set(fn, "arguments", json_str(tc.arguments));
            Json entry = json_obj();
            obj_set(entry, "id", json_str(tc.id));
            obj_set(entry, "type", json_str("function"));
            obj_set(entry, "function", fn);
            arr_push(arr, entry);
        }
        obj_set(o, "tool_calls", arr);
    }
    if (!m.tool_call_id.empty()) obj_set(o, "tool_call_id", json_str(m.tool_call_id));
    if (!m.name.empty()) obj_set(o, "name", json_str(m.name));
    if (!m.finish_reason.empty()) obj_set(o, "finish_reason", json_str(m.finish_reason));
    if (!m.reasoning.empty()) obj_set(o, "reasoning", json_str(m.reasoning));
    return o;
}

Message message_from_json(const Json& j) {
    Message m;
    if (j.type != Json::Obj) return m;
    if (const Json* r = json_get(j, "role")) m.role = r->type == Json::Str ? r->str : "";
    if (const Json* c = json_get(j, "content"))
        m.content = c->type == Json::Str ? c->str : (c->type == Json::Null ? "" : json_dump(*c));
    if (const Json* tcid = json_get(j, "tool_call_id"))
        m.tool_call_id = tcid->type == Json::Str ? tcid->str : "";
    if (const Json* nm = json_get(j, "name")) m.name = nm->type == Json::Str ? nm->str : "";
    if (const Json* fr = json_get(j, "finish_reason"))
        m.finish_reason = fr->type == Json::Str ? fr->str : "";
    if (const Json* rs = json_get(j, "reasoning"))
        m.reasoning = rs->type == Json::Str ? rs->str : "";
    if (const Json* tcs = json_get(j, "tool_calls")) {
        if (tcs->type == Json::Arr) {
            for (const auto& esp : tcs->arr) {
                const Json& e = *esp;
                if (e.type != Json::Obj) continue;
                ToolCall tc;
                if (const Json* id = json_get(e, "id")) tc.id = id->type == Json::Str ? id->str : "";
                const Json* fn = json_get(e, "function");
                if (!fn) {
                    // Some wire forms put name/arguments flat on the entry.
                    if (const Json* n = json_get(e, "name")) tc.name = n->type == Json::Str ? n->str : "";
                    if (const Json* a = json_get(e, "arguments"))
                        tc.arguments = a->type == Json::Str ? a->str : json_dump(*a);
                } else if (fn->type == Json::Obj) {
                    if (const Json* n = json_get(*fn, "name")) tc.name = n->type == Json::Str ? n->str : "";
                    if (const Json* a = json_get(*fn, "arguments"))
                        tc.arguments = a->type == Json::Str ? a->str : json_dump(*a);
                }
                m.tool_calls.push_back(tc);
            }
        }
    }
    return m;
}

}  // namespace core
}  // namespace dd
