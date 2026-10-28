#pragma once
// dd/agent/agent.hpp — the agent run loop (port of run_agent.py AIAgent core).
//
// Faithful to the Python loop semantics:
//   while api_call_count < max_iterations and budget.remaining > 0:
//     interrupt check -> break
//     api_call_count += 1; if not budget.consume(): break
//     build payload; call LLM client; normalize -> assistant msg
//     append assistant
//     if tool_calls: dedup + cap_delegate (before the message is recorded) + execute (seq/concurrent) -> append tool results
//     else: final response; break
//   on hitting the cap: _handle_max_iterations (one summary call).
// Every exit of run() closes the context engine's turn once (end_turn), a throwing LLM call included.
//
// The provider HTTP transport is a pluggable LLMClient interface; the LOOP
// LOGIC is what is ported faithfully here.

#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <dd/core/message.hpp>
#include <dd/core/json.hpp>
#include <atomic>
#include <functional>
#include <dd/tools/registry.hpp>
#include <dd/agent/iteration_budget.hpp>

namespace dd {
namespace agent {

// Provider transport. Implementations do the real HTTP; the loop calls chat().
struct LLMClient {
    virtual ~LLMClient() = default;
    // Return the assistant message (content + tool_calls + finish_reason).
    virtual core::Message chat(const std::vector<core::Message>& messages,
                               const core::Json& tools) = 0;
};

// What the loop sends, owned by someone who remembers across turns. Without
// one, run() sends its own per-turn transcript (the original port). With one,
// every request is the engine's payload (dd/context: the tape), and the loop
// reports what it appends so the engine can keep the history.
struct ContextEngine {
    virtual ~ContextEngine() = default;
    virtual void begin_turn(const std::string& user_message) = 0;
    virtual std::vector<core::Message> payload() = 0;   // system prompt included
    virtual core::Json tools() = 0;
    virtual void on_assistant(const core::Message& m) = 0;
    virtual void on_tool_results(const std::vector<core::Message>& results) = 0;
    virtual void after_tools() = 0;                     // between calls: watermarks
    // Input the operator typed while the turn was running, folded in at a round
    // boundary: already role "user", appended after this round's tool results and
    // before the next call, so the newest words are the last thing the model reads.
    // The engine records it as mid-turn, not as the turn's question: a tape that
    // cannot tell when input arrived cannot tell what the model was asked.
    virtual void on_steering(const std::vector<core::Message>& notes) = 0;
    virtual void end_turn(const std::string& final_response, bool interrupted) = 0;
    // The server refused a request for the context window (what() of the failed call). true: the history was
    // rebuilt to fit and the call goes out again, once; false: the error stands.
    virtual bool on_overflow(const std::string& /*error*/) { return false; }
    // The loop's ephemeral prompt (the plan), handed over before every call. true: the engine keeps it
    // where the cached prefix survives it; false: the loop sends it behind the payload.
    virtual bool take_ephemeral(const std::string& /*prompt*/) { return false; }
};

// A rule that can refuse a tool call before it runs. veto() returns the tool result the model gets
// instead (the call is not executed), or nullopt: the call may run. Called on the agent thread, in
// order, before any call of the batch starts.
struct ToolGate {
    virtual ~ToolGate() = default;
    virtual std::optional<std::string> veto(const core::ToolCall& call) = 0;
};

class Agent {
public:
    // ---- configuration ----
    std::string model;
    int max_iterations = 90;
    double tool_delay = 0.0;   // seconds between sequential calls of one batch; an interrupt ends the wait
    std::vector<std::string> enabled_toolsets, disabled_toolsets;
    bool quiet_mode = true;
    std::string system_prompt;
    std::string ephemeral_system_prompt;
    // Budget-pressure thresholds (run_agent.py defaults).
    bool budget_pressure_enabled = true;
    double budget_warning_threshold = 0.9;
    double budget_caution_threshold = 0.75;

    // Streaming hook: called for every assistant + tool-result message the
    // loop appends. A TUI sets it to a colored per-step printer; the loop
    // logic is otherwise untouched.
    std::function<void(const core::Message&)> step_hook;

    // Pull hook at the round boundary. The owner hands back the prompts typed since
    // the last pull and removes them; empty means nothing was typed. Runs on the
    // agent thread, so the owner owns the lock, not the loop. An interrupt stops the
    // pull — it does not discard what was typed, which the idle loop still sends.
    std::function<std::vector<std::string>()> steer_hook;

    // ---- collaborators (not owned) ----
    tools::ToolRegistry* registry = nullptr;
    LLMClient* llm = nullptr;
    ContextEngine* context = nullptr;
    ToolGate* gate = nullptr;

    // ---- owned state ----
    std::unique_ptr<IterationBudget> budget;
    int api_call_count = 0;
    // Set from another thread (a TUI's Esc); the loop polls it between steps.
    std::atomic<bool> interrupt_requested{false};

    // ---- run loop ----
    std::string run(const std::string& user_message);

    void execute_tool_calls(const core::Message& assistant,
                            std::vector<core::Message>& messages,
                            int api_call_count);

    std::string handle_max_iterations(std::vector<core::Message>& messages,
                                      int api_call_count);

private:
    std::string run_loop(const std::string& user_message, bool& interrupted);
};

// ---- faithful helpers (namespace-level) ----
bool should_parallelize(const std::vector<core::ToolCall>& calls, tools::ToolRegistry* reg);
std::vector<core::ToolCall> dedup_tool_calls(const std::vector<core::ToolCall>& calls);
std::vector<core::ToolCall> cap_delegate_calls(const std::vector<core::ToolCall>& calls, int max_children);
std::string get_budget_warning(int api_call_count, int max_iterations,
                               double warn_threshold, double caution_threshold);

}  // namespace agent
}  // namespace dd
