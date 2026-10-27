// src/agent/agent.cpp — the agent run loop + faithful helpers.
#include "dd/agent/agent.hpp"
#include "dd/agent/iteration_budget.hpp"
#include "dd/core/message.hpp"
#include "dd/core/json.hpp"
#include "dd/tools/registry.hpp"

#include <algorithm>
#include <thread>
#include <chrono>
#include <filesystem>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace dd {
namespace agent {

namespace {
// Parallel-safety sets (run_agent.py:246-277).
const std::vector<std::string> NEVER_PARALLEL = {"clarify"};
const std::vector<std::string> PARALLEL_SAFE = {
    "ha_get_state", "ha_list_entities", "ha_list_services",
    "mazemaker_graph", "mazemaker_recall", "mazemaker_think",
    "read_file", "search_files", "session_search", "skill_view",
    "skills_list", "vision_analyze", "web_extract", "web_search"};
const std::vector<std::string> PATH_SCOPED = {"read_file", "write_file", "patch"};
constexpr int MAX_TOOL_WORKERS = 8;

bool in_set(const std::vector<std::string>& s, const std::string& v) {
    return std::find(s.begin(), s.end(), v) != s.end();
}

// _paths_overlap: equal, or one is an ancestor directory of the other.
bool paths_overlap(const std::string& a, const std::string& b) {
    if (a == b) return true;
    const std::string ap = a + "/";
    const std::string bp = b + "/";
    return ap.rfind(bp, 0) == 0 || bp.rfind(ap, 0) == 0;
}

// _extract_parallel_scope_path + expanduser/abspath normalization.
std::string norm_path(std::string p) {
    if (!p.empty() && p[0] == '~') {
        if (const char* h = std::getenv("HOME")) p = std::string(h) + p.substr(1);
    }
    std::filesystem::path pp(p);
    if (!pp.is_absolute()) pp = std::filesystem::current_path() / pp;
    return pp.string();
}

// The model ran out of output budget mid-arguments: the call must NOT run
// (a cut write_file would store half a file), so answer as a tool error the
// model can act on; the turn keeps going.
std::string bad_args_json(const core::Json& args, bool repaired) {
    if (repaired && args.type == core::Json::Obj)
        return "{\"error\":\"Tool arguments were cut off by the model's output budget. Retry with a smaller payload: write the file in pieces.\"}";
    return "{\"error\":\"Tool arguments are not valid JSON.\"}";
}
}  // anon

// ---- should_parallelize (run_agent.py:305) ----
bool should_parallelize(const std::vector<core::ToolCall>& calls, tools::ToolRegistry*) {
    if (calls.size() <= 1) return false;
    for (const auto& tc : calls)
        if (in_set(NEVER_PARALLEL, tc.name)) return false;

    std::vector<std::string> reserved;
    for (const auto& tc : calls) {
        bool repaired = false;
        core::Json args = core::json_parse_args(tc.arguments, repaired);
        if (args.type != core::Json::Obj || repaired) return false;  // bad/cut args -> run it alone
        if (in_set(PATH_SCOPED, tc.name)) {
            const core::Json* p = core::json_get(args, "path");
            if (!p || p->type != core::Json::Str || p->str.empty()) return false;
            std::string norm = norm_path(p->str);
            for (const auto& r : reserved)
                if (paths_overlap(norm, r)) return false;
            reserved.push_back(norm);
            continue;
        }
        if (!in_set(PARALLEL_SAFE, tc.name)) return false;
    }
    return true;
}

// ---- dedup_tool_calls (run_agent.py:5507) ----
std::vector<core::ToolCall> dedup_tool_calls(const std::vector<core::ToolCall>& calls) {
    std::vector<core::ToolCall> out;
    std::vector<std::pair<std::string, std::string>> seen;
    for (const auto& tc : calls) {
        std::pair<std::string, std::string> key{tc.name, tc.arguments};
        bool dup = false;
        for (const auto& s : seen) if (s == key) { dup = true; break; }
        if (!dup) { seen.push_back(key); out.push_back(tc); }
    }
    return out;
}

// ---- cap_delegate_calls (run_agent.py:5477) ----
std::vector<core::ToolCall> cap_delegate_calls(const std::vector<core::ToolCall>& calls, int max_children) {
    int delegates = 0;
    for (const auto& tc : calls) if (tc.name == "delegate_task") ++delegates;
    if (delegates <= max_children) return calls;
    std::vector<core::ToolCall> out;
    int kept = 0;
    for (const auto& tc : calls) {
        if (tc.name == "delegate_task") {
            if (kept < max_children) { out.push_back(tc); ++kept; }
        } else {
            out.push_back(tc);
        }
    }
    return out;
}

// ---- get_budget_warning (run_agent.py:9524) ----
std::string get_budget_warning(int api_call_count, int max_iterations,
                               double warn_threshold, double caution_threshold) {
    if (max_iterations <= 0) return "";
    double progress = static_cast<double>(api_call_count) / max_iterations;
    int remaining = max_iterations - api_call_count;
    if (progress >= warn_threshold)
        return "[BUDGET WARNING: Iteration " + std::to_string(api_call_count) + "/" +
               std::to_string(max_iterations) + ". Only " + std::to_string(remaining) +
               " iteration(s) left. Provide your final response NOW. No more tool calls unless absolutely critical.]";
    if (progress >= caution_threshold)
        return "[BUDGET: Iteration " + std::to_string(api_call_count) + "/" +
               std::to_string(max_iterations) + ". " + std::to_string(remaining) +
               " iterations left. Start consolidating your work.]";
    return "";
}

// ---- execute_tool_calls (run_agent.py:8880) ----
// Every call of the assistant message gets exactly one result: run, refused by the gate, or cancelled by an interrupt
// (the calls not yet started when the flag is set). A call that has started finishes.
void Agent::execute_tool_calls(const core::Message& assistant, std::vector<core::Message>& messages, int api_call_count) {
    const std::vector<core::ToolCall>& calls = assistant.tool_calls;
    const size_t n = calls.size();
    std::vector<std::string> results(n);
    const auto cancelled = [](const core::ToolCall& tc) {
        return "[Tool execution cancelled — " + tc.name + " was skipped due to user interrupt]";
    };

    // a gate may refuse a call: decided up front, in order, before anything runs
    std::vector<std::optional<std::string>> vetoed(n);
    if (gate && !interrupt_requested.load(std::memory_order_acquire))
        for (size_t i = 0; i < n; ++i) vetoed[i] = gate->veto(calls[i]);

    const auto run_one = [&](size_t i) {
        bool repaired = false;
        const core::Json args = core::json_parse_args(calls[i].arguments, repaired);
        if (registry && (repaired || args.type != core::Json::Obj))
            results[i] = bad_args_json(args, repaired);
        else
            results[i] = registry ? registry->dispatch(calls[i].name, args)
                                  : std::string("{\"error\":\"Unknown tool: ") + calls[i].name + "\"}";
    };

    if (should_parallelize(calls, registry)) {
        for (size_t i = 0; i < n;) {
            const size_t batch = std::min<size_t>(MAX_TOOL_WORKERS, n - i);
            std::vector<std::thread> workers;
            for (size_t k = 0; k < batch; ++k) {
                const size_t idx = i + k;
                if (interrupt_requested.load(std::memory_order_acquire)) {
                    results[idx] = cancelled(calls[idx]);
                    continue;
                }
                if (vetoed[idx]) {
                    results[idx] = *vetoed[idx];
                    continue;
                }
                workers.emplace_back([&, idx] {
                    if (interrupt_requested.load(std::memory_order_acquire)) results[idx] = cancelled(calls[idx]);
                    else run_one(idx);
                });
            }
            for (auto& t : workers) t.join();
            i += batch;
        }
    } else {
        for (size_t i = 0; i < n; ++i) {
            if (interrupt_requested.load(std::memory_order_acquire)) {
                results[i] = cancelled(calls[i]);
                continue;
            }
            if (vetoed[i]) {
                results[i] = *vetoed[i];
                continue;
            }
            run_one(i);
            if (tool_delay > 0 && i + 1 < n) {   // slices, so an interrupt ends the wait within one
                const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(tool_delay);
                while (!interrupt_requested.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < end)
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
    }

    for (size_t i = 0; i < n; ++i) {
        core::Message tm;
        tm.role = "tool";
        tm.content = results[i];
        tm.tool_call_id = calls[i].id;
        messages.push_back(tm);
        if (step_hook) step_hook(tm);
    }
    (void)api_call_count;
}

namespace {
// The ephemeral prompt rides every call exactly once and is never stored in `messages`:
// under a tape it goes after the rendered payload, so the cached prefix stays
// byte-identical and only the part after it changes; without a tape the payload is
// rebuilt anyway, so it sits behind the standing system prompt.
void attach_ephemeral(std::vector<core::Message>& payload, const std::string& prompt,
                      bool under_tape, bool standing_system) {
    if (prompt.empty()) return;
    core::Message ep;
    ep.role = "system";
    ep.content = prompt;
    if (under_tape) payload.push_back(ep);
    else payload.insert(payload.begin() + (standing_system ? 1 : 0), ep);
}
}  // namespace

// ---- handle_max_iterations (run_agent.py:9617) ----
std::string Agent::handle_max_iterations(std::vector<core::Message>& messages, int api_call_count) {
    (void)api_call_count;
    core::Message req;
    req.role = "user";
    req.content = "You've reached the maximum number of tool-calling iterations allowed. "
                  "Please provide a final response summarizing what you've found and accomplished so far, "
                  "without calling any more tools.";
    messages.push_back(req);

    if (!llm) return "I reached the iteration limit and couldn't generate a summary.";
    try {
        core::Json tools = core::json_arr();  // no tools on the summary call
        std::vector<core::Message> sent = messages;   // req is already in messages
        if (context) {   // the tape plus the plan plus the request: everything before it is already cached
            sent = context->payload();
            attach_ephemeral(sent, ephemeral_system_prompt, true, false);
            sent.push_back(req);
            tools = context->tools();   // unchanged, or the server re-reads the whole prefix
        } else {
            attach_ephemeral(sent, ephemeral_system_prompt, false, !system_prompt.empty());
        }
        core::Message summary = llm->chat(sent, tools);
        std::string text = summary.content;
        if (!text.empty()) {
            core::Message am;
            am.role = "assistant";
            am.content = text;
            am.finish_reason = "stop";
            messages.push_back(am);
            if (context) context->on_assistant(am);
            return text;
        }
        if (summary.has_tool_calls()) {
            std::string names;
            for (const auto& tc : summary.tool_calls) names += (names.empty() ? "" : ", ") + tc.name;
            return "I reached the maximum iterations (" + std::to_string(max_iterations) +
                   ") and the model tried to call tools (" + names + ") instead of summarising: no summary was produced.";
        }
        return "I reached the iteration limit and couldn't generate a summary.";
    } catch (...) {
        return "I reached the maximum iterations (" + std::to_string(max_iterations) +
               ") but couldn't summarize. Error: transport failure";
    }
}

// ---- run loop (run_agent.py:10202) ----
// Whatever ends the turn, the engine's turn is closed once: a call that throws (an Esc during generation) included.
std::string Agent::run(const std::string& user_message) {
    if (context) context->begin_turn(user_message);
    bool interrupted = false;
    std::string final_response;
    try {
        final_response = run_loop(user_message, interrupted);
    } catch (...) {
        if (context) {
            try {
                context->end_turn("", true);
            } catch (...) {
            }
        }
        throw;
    }
    if (context) context->end_turn(final_response, interrupted || interrupt_requested.load());
    return final_response;
}

std::string Agent::run_loop(const std::string& user_message, bool& interrupted) {
    std::vector<core::Message> messages;
    if (!system_prompt.empty()) {
        core::Message sys;
        sys.role = "system";
        sys.content = system_prompt;
        messages.push_back(sys);
    }
    {
        core::Message um;
        um.role = "user";
        um.content = user_message;
        messages.push_back(um);
    }

    budget = std::make_unique<IterationBudget>(max_iterations);
    api_call_count = 0;
    std::string final_response;

    while (api_call_count < max_iterations && budget->remaining() > 0) {
        if (interrupt_requested) { interrupted = true; break; }

        api_call_count += 1;
        if (!budget->consume()) {
            if (!quiet_mode)
                std::cerr << "\nIteration budget exhausted (" << budget->used.load() << "/"
                          << budget->max_total << " iterations used)\n";
            break;
        }

        std::vector<core::Message> payload = messages;
        core::Json tools = core::json_arr();
        if (context) {
            payload = context->payload();
            tools = context->tools();
        } else if (registry) {
            for (auto& d : registry->get_definitions(registry->get_all_tool_names()))
                core::arr_push(tools, d);
        }
        attach_ephemeral(payload, ephemeral_system_prompt, context != nullptr, !system_prompt.empty());

        core::Message assistant;
        if (llm) {
            for (bool again = false;;) {   // refused for the window: rebuilt by the engine, sent once more
                try {
                    assistant = llm->chat(payload, tools);
                    break;
                } catch (const std::exception& e) {
                    if (again || !context || !context->on_overflow(e.what())) throw;
                    again = true;
                    payload = context->payload();
                    tools = context->tools();
                }
            }
        }
        if (assistant.role.empty()) assistant.role = "assistant";
        // a dropped duplicate leaves the message too, so no tool_call is ever recorded without its result
        assistant.tool_calls = cap_delegate_calls(dedup_tool_calls(assistant.tool_calls), 3);
        if (assistant.finish_reason.empty())
            assistant.finish_reason = assistant.has_tool_calls() ? "tool_calls" : "stop";

        messages.push_back(assistant);
        if (context) context->on_assistant(assistant);
        if (step_hook) step_hook(assistant);

        if (!assistant.has_tool_calls()) {
            final_response = assistant.content;
            break;
        }

        const size_t before = messages.size();
        execute_tool_calls(assistant, messages, api_call_count);
        if (context) {
            context->on_tool_results(std::vector<core::Message>(messages.begin() + static_cast<long>(before), messages.end()));
            context->after_tools();
        }

        // Mid-turn steering, after the round's results and the router's note: the
        // operator's newest words enter the next call, so a turn can be redirected
        // without waiting for it to finish.
        if (steer_hook && !interrupt_requested.load(std::memory_order_acquire)) {
            const std::vector<std::string> typed = steer_hook();
            if (!typed.empty()) {
                std::vector<core::Message> notes;
                for (const std::string& t : typed) {
                    core::Message m;
                    m.role = "user";
                    m.content = t;
                    notes.push_back(m);
                    messages.push_back(std::move(m));
                }
                if (context) context->on_steering(notes);
            }
        }
    }

    if (final_response.empty() && !interrupted && api_call_count >= max_iterations)
        final_response = handle_max_iterations(messages, api_call_count);
    return final_response;
}

}  // namespace agent
}  // namespace dd
