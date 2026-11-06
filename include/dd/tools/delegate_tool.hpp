#pragma once
// dd/tools/delegate_tool.hpp — sub-agents: the parent side of delegate_task.
// Port of tools/delegate_tool.py (the spawn/run/report half; the child is the
// dd::agent::Agent loop itself, not a port of it).
//
// Shape, faithful to the Python tool:
//   delegate_task(goal, context?, toolsets?, max_iterations?)   one child
//   delegate_task(tasks=[{goal, context?, toolsets?}, ...], ...) up to
//                kDelegateMaxChildren children, run concurrently
//   -> {"results":[{task_index,status,summary,api_calls,duration_seconds,
//                   exit_reason,tokens,...}, ...],"total_duration_seconds":N}
// A child is a fresh Agent with its own transcript, its own HttpLLM (the wire
// config copied from the parent's transport) and its own tool registry (the
// file and terminal tools; the todo ledger is the child's own). Depth is
// bounded: a child one hop deeper than its parent, delegation ends at
// kDelegateMaxDepth. The parent's ToolGate stays in charge: a child's calls
// are vetoed by the same gate, so a device paired without local tools cannot
// reach them through a child.

#include <functional>
#include <memory>
#include <string>

#include "dd/core/json.hpp"
#include "dd/tools/shell.hpp"

namespace dd {
namespace agent {
class Agent;
class HttpLLM;
class LLMClient;
}  // namespace agent

namespace tools {

constexpr int kDelegateMaxChildren = 3;    // delegate_tool.py MAX_CONCURRENT_CHILDREN
constexpr int kDelegateMaxDepth = 2;       // delegate_tool.py MAX_DEPTH
constexpr int kDelegateMaxIterations = 50; // delegate_tool.py DEFAULT_MAX_ITERATIONS

// The focused prompt a child runs on (delegate_tool.py::_build_child_system_prompt).
// `workspace_path` empty means the paragraph is left out.
std::string delegate_child_prompt(const std::string& goal, const std::string& context,
                                  const std::string& workspace_path);

// The delegate_task tool schema, for register_tool at the call site.
core::Json delegate_tool_schema();

struct DelegateToolDeps {
    agent::Agent* parent = nullptr;             // depth source; must outlive the handler
    const agent::HttpLLM* transport = nullptr;  // wire config the children copy
    // In-process override (tests, and a harness that owns its own client): a
    // child's transport comes from here, one fresh client per child, instead
    // of a copy of `transport`. Set: `transport` may be null. Called once per
    // task on the handler's own thread, before any child thread starts.
    std::function<std::shared_ptr<agent::LLMClient>()> llm_factory;
    std::string workspace_hint;                 // absolute dir named in the child prompt; "" = none
    std::string skills_root;                    // edit_file's learned-skill annotation root; "" = none
    ShellPolicy shell_policy;                   // the same denylist the parent's terminal runs under
};

// The delegate_task handler. Never throws: every failure is a result entry or
// a {"error":...} tool result, exactly as the Python tool answers.
std::function<std::string(const core::Json&)> make_delegate_tool(const DelegateToolDeps& deps);

}  // namespace tools
}  // namespace dd
