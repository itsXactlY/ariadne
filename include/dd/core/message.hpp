#pragma once
// dd/core/message.hpp — the wire message model (run_agent.py message dicts).
//
// A message is a dict in Python: {role, content, tool_calls?, tool_call_id?,
// name?, finish_reason?, reasoning?}. Roles: system|user|assistant|tool.
// An assistant message may carry tool_calls; a tool message answers one via
// tool_call_id. tool_calls entries are {id, type:"function",
// function:{name, arguments}} on the wire.

#include <string>
#include <vector>
#include <cstddef>
#include <dd/core/json.hpp>

namespace dd {
namespace core {

struct ToolCall {
    std::string id;          // call id (deterministic when provider omits it)
    std::string name;        // function.name
    std::string arguments;   // function.arguments (JSON string)
};

struct Message {
    std::string role;                 // system|user|assistant|tool
    std::string content;              // may be empty
    std::vector<ToolCall> tool_calls; // assistant only
    std::string tool_call_id;         // tool-result only: answers this call
    std::string name;                 // optional
    std::string finish_reason;        // assistant: "tool_calls"|"stop"|...
    std::string reasoning;            // assistant reasoning trace (optional)

    bool has_tool_calls() const { return !tool_calls.empty(); }
};

// Serialize a message to the OpenAI Chat-Completions wire form.
Json message_to_json(const Message& m);

// Parse a wire message (dict form) back into the model.
Message message_from_json(const Json& j);

// THE WIRE INVARIANT: exactly one system message, and it is index 0.
//
// A chat template is free to refuse anything else, and some do: Qwen3.5's raises "System message
// must be at the beginning." for any system role past the first (tokenizer.chat_template line 83),
// which llama-server reports as HTTP 500 — the whole turn is lost. The harness has a legitimate
// use for system-role text mid-conversation (plan revisions, compression notices) and those are
// authored as hidden user items now, but a rule held only by convention is one refactor away from
// returning. So the payload is normalized here, on the way to the wire: every system message's
// content is merged, in order, into a single head message; if the list had none, the merged text
// becomes the head. Returns how many messages were merged away (0 = the list was already valid,
// which is the normal case and costs one scan).
std::size_t collapse_system_messages(std::vector<Message>& msgs);

}  // namespace core
}  // namespace dd
