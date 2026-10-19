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

}  // namespace core
}  // namespace dd
