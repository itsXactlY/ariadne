// Port of tools/delegate_tool.py — the parent side of sub-agents.
// The child is a fresh dd::agent::Agent (the loop is already ported there);
// this file is the spawn, the focused prompt, the depth/width limits, the
// concurrent batch and the structured report the parent's model reads.
#include "dd/tools/delegate_tool.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "dd/agent/agent.hpp"
#include "dd/agent/http_llm.hpp"
#include "dd/tools/interrupt.hpp"
#include "dd/tools/fs_tools.hpp"
#include "dd/tools/patch_apply.hpp"
#include "dd/tools/registry.hpp"
#include "dd/tools/skill_provenance.hpp"
#include "dd/tools/todo_tool.hpp"

using namespace dd::core;

namespace dd {
namespace tools {

namespace {

JsonSP jsp(Json v) { return std::make_shared<Json>(std::move(v)); }

// ---- the child's tools: the file and terminal set, every handler thin over the
// shared primitives (fs_read/fs_write/fs_list/fs_edit, run_shell/start_job,
// job_call) exactly as the thin handlers in the two mains are. Children do not
// share the parent's TodoStore (a sub-agent's plan ledger is its own) and do
// not write the parent's ephemeral prompt. ----

struct ChildToolDeps {
    std::string skills_root;
    ShellPolicy shell_policy;
    int child_depth = 0;                     // delegate_task registers only under the ceiling
    const DelegateToolDeps* delegate = nullptr;
    const std::set<std::string>* filter = nullptr;   // requested toolsets; empty = all
    bool wanted(const char* toolset) const {
        return !filter || filter->empty() || filter->count(toolset);
    }
};

void register_child_tools(ToolRegistry& reg, const ChildToolDeps& d) {
    ToolEntry e;
    if (d.wanted("file")) {
        e.name = "read_file";
        e.toolset = "file";
        e.schema = fs_tool_schema({"path"}, "Read a file's UTF-8 text content.");
        e.handler = [](const Json& a) { return fs_read(a); };
        reg.register_tool(e);

        e.name = "write_file";
        e.schema = fs_tool_schema({"path", "content"}, "Write text content to a file.");
        e.handler = [](const Json& a) { return fs_write(a); };
        reg.register_tool(e);

        e.name = "list_dir";
        e.schema = fs_tool_schema({"path"}, "List entries of a directory.");
        e.handler = [](const Json& a) { return fs_list(a); };
        reg.register_tool(e);

        e.name = "edit_file";
        e.schema = edit_tool_schema();
        e.handler = [skills_root = d.skills_root](const Json& a) {
            ApplyOptions opt;
            opt.skills_root = skills_root;
            const WriteOriginScope scope("model_edit");
            return fs_edit(a, opt);
        };
        reg.register_tool(e);
    }

    if (d.wanted("terminal")) {
        e.name = "run_terminal";
        e.toolset = "terminal";
        Json rt = json_arr();
        arr_push(rt, json_str("cmd"));
        Json props = json_obj({{"cmd", jsp(json_obj({{"type", jsp(json_str("string"))},
                                                     {"description", jsp(json_str("command to run"))}}))},
                               {"background", jsp(json_obj({{"type", jsp(json_str("boolean"))},
                                                            {"description", jsp(json_str("true: start it and return at once with a job id"))}}))},
                               {"intent", jsp(json_obj({{"type", jsp(json_str("string"))},
                                                        {"description", jsp(json_str("what you are doing, in a few words (optional)"))}}))}});
        e.schema = json_obj({{"description", jsp(json_str(
            "Run a shell command. A guard refuses obviously destructive commands; it is not a sandbox. "
            "Returns exit code + stdout. A foreground command is killed after "
            + std::to_string(kForegroundTimeoutMs / 1000) + " s; long work runs with background:true and is "
            "looked at with the job tool."))},
                             {"parameters", jsp(json_obj({{"type", jsp(json_str("object"))},
                                                          {"properties", jsp(std::move(props))},
                                                          {"required", jsp(std::move(rt))}}))}});
        e.handler = [policy = d.shell_policy](const Json& a) {
        const Json* c = json_get(a, "cmd");
        if (!c || c->type != Json::Str || c->str.empty()) return tool_error("cmd required");
        const Json* bg = json_get(a, "background");
        if (bg && bg->type == Json::Bool && bg->b) return with_job_notices(start_job(c->str, policy));
        ShellOptions opt;
        opt.cancel = interrupt::current();
        return with_job_notices(run_shell(c->str, opt, policy));
    };
        reg.register_tool(e);

        e.name = "job";
        e.toolset = "terminal";
        Json actions = json_arr();
        for (const char* a : {"status", "wait", "kill", "list"}) arr_push(actions, json_str(a));
        Json jprops = json_obj({{"action", jsp(json_obj({{"type", jsp(json_str("string"))},
                                                        {"enum", jsp(std::move(actions))}}))},
                               {"job", jsp(json_obj({{"type", jsp(json_str("integer"))},
                                                     {"description", jsp(json_str("the id run_terminal returned"))}}))},
                               {"wait_s", jsp(json_obj({{"type", jsp(json_str("integer"))}}))}});
        Json rq = json_arr();
        arr_push(rq, json_str("action"));
        e.schema = json_obj({{"description", jsp(json_str("Look at a background command started with run_terminal background:true."))},
                             {"parameters", jsp(json_obj({{"type", jsp(json_str("object"))},
                                                          {"properties", jsp(std::move(jprops))},
                                                          {"required", jsp(std::move(rq))}}))}});
        e.handler = [](const Json& a) {
        auto str = [&](const char* k) {
            const Json* v = json_get(a, k);
            return v && v->type == Json::Str ? v->str : std::string();
        };
        auto num = [&](const char* k) {
            const Json* v = json_get(a, k);
            return v && v->type == Json::Num ? static_cast<int>(v->num) : 0;
        };
        JobOptions opt;
        opt.cancel = interrupt::current();
        return with_job_notices(job_call(str("action"), num("job"), num("wait_s"), opt));
    };
        reg.register_tool(e);
    }

    e.name = "todo";
    e.toolset = "";   // an agent-loop tool, not a toolset the caller filters
    {
        Json item = json_obj({{"id", jsp(json_obj({{"type", jsp(json_str("string"))}}))},
                              {"content", jsp(json_obj({{"type", jsp(json_str("string"))}}))},
                              {"status", jsp(json_obj({{"type", jsp(json_str("string"))},
                                                       {"enum", jsp(json_arr({jsp(json_str("pending")), jsp(json_str("in_progress")),
                                                                              jsp(json_str("completed")), jsp(json_str("cancelled"))}))}}))}});
        Json props = json_obj({{"todos", jsp(json_obj({{"type", jsp(json_str("array"))},
                                                       {"items", jsp(std::move(item))}}))},
                               {"merge", jsp(json_obj({{"type", jsp(json_str("boolean"))}}))}});
        e.schema = json_obj({{"description", jsp(json_str("Work ledger for a multi-step task. The sub-agent's own ledger: "
                                                          "provide `todos` to write the whole list, omit to read it back; "
                                                          "merge=true updates only the items named."))},
                             {"parameters", jsp(json_obj({{"type", jsp(json_str("object"))},
                                                          {"properties", jsp(std::move(props))}}))}});
    }
    e.handler = [store = std::make_shared<TodoStore>()](const Json& a) {
        const Json* t = json_get(a, "todos");
        const Json* m = json_get(a, "merge");
        return todo_tool(t ? *t : Json{}, todo_merge_flag(m), *store);
    };
    reg.register_tool(e);

    // Delegation, one hop under the ceiling: a child at depth kDelegateMaxDepth
    // gets no delegate_task, so the tree ends there (delegate_tool.py checks
    // parent depth at call time; here the tool simply is not there to call).
    if (d.child_depth < kDelegateMaxDepth && d.delegate) {
        e.name = "delegate_task";
        e.toolset = "";   // agent-loop tool: never filtered away (delegate_tool.py _AGENT_LOOP_TOOLS)
        e.schema = delegate_tool_schema();
        e.handler = make_delegate_tool(*d.delegate);
        reg.register_tool(e);
    }
}

// ---- one child, start to report ----

Json child_entry(int task_index, const std::string& status, const std::string& summary,
                 int api_calls, double duration_s, const std::string& model,
                 const std::string& exit_reason, long prompt_tokens, long completion_tokens,
                 const std::string& error) {
    Json t = json_obj({{"input", jsp(json_num(static_cast<double>(prompt_tokens)))},
                       {"output", jsp(json_num(static_cast<double>(completion_tokens)))}});
    Json e = json_obj({{"task_index", jsp(json_num(task_index))},
                       {"status", jsp(json_str(status))},
                       {"summary", summary.empty() ? jsp(Json{}) : jsp(json_str(summary))},
                       {"api_calls", jsp(json_num(api_calls))},
                       {"duration_seconds", jsp(json_num(std::round(duration_s * 100.0) / 100.0))},
                       {"model", model.empty() ? jsp(Json{}) : jsp(json_str(model))},
                       {"exit_reason", jsp(json_str(exit_reason))},
                       {"tokens", jsp(std::move(t))}});
    if (!error.empty()) obj_set(e, "error", json_str(error));
    return e;
}

Json run_one_child(int task_index, const std::string& goal, const std::string& context,
                   int child_depth, int max_iterations, const DelegateToolDeps& deps,
                   const std::set<std::string>& toolsets,
                   std::shared_ptr<agent::LLMClient> assigned) {
    using clock = std::chrono::steady_clock;
    const clock::time_point t0 = clock::now();

    if (interrupt::current() && interrupt::is_interrupted())
        return child_entry(task_index, "interrupted", "", 0, 0.0, "", "interrupted", 0, 0, "");

    // Fresh child: own transcript (no ContextEngine), own transport and registry.
    agent::Agent child;
    agent::HttpLLM llm;
    std::atomic<long> prompt_tokens{0}, completion_tokens{0};
    if (assigned) {
        child.llm = assigned.get();   // owned by the handler; outlives the child's run
    } else if (deps.transport) {
        llm.base_url = deps.transport->base_url;
        llm.model = deps.transport->model;
        llm.api_key = deps.transport->api_key;
        llm.extra = json_deep_copy(deps.transport->extra);
        llm.effort = deps.transport->effort;
        llm.shape = deps.transport->shape;
        llm.attribution = deps.transport->attribution;
        llm.stall_timeout_s = deps.transport->stall_timeout_s;
        llm.retry_attempts = deps.transport->retry_attempts;
        llm.retry_budget_ms = deps.transport->retry_budget_ms;
        llm.retry_after_cap_ms = deps.transport->retry_after_cap_ms;
        llm.retry_after_total_ms = deps.transport->retry_after_total_ms;
        // NOT on_wait: the parent's closure captures the TUI's scene by reference,
        // and a child runs on a worker thread — three children waiting on their wire
        // (a 429, a stall, a backoff) would mutate the parent's scene concurrently
        // and shred the heap; the next std::thread spawn then dies reading freed
        // state (the parallel-batch segv of 2026-11-06). A child is quiet_mode:
        // its waits are internal, they paint nothing.
    }
    llm.cancel = interrupt::current();   // the parent's Esc kills the child's wire call too
    llm.on_stats = [&prompt_tokens, &completion_tokens](const agent::CallStats& s) {
        prompt_tokens += s.prompt_tokens;
        completion_tokens += s.completion_tokens;
    };

    ToolRegistry reg;
    DelegateToolDeps child_deps = deps;                 // the grandchild's wire is the same
    child_deps.parent = &child;
    ChildToolDeps ct{deps.skills_root, deps.shell_policy, child_depth, &child_deps, &toolsets};
    register_child_tools(reg, ct);

    child.model = assigned ? (deps.parent ? deps.parent->model : std::string()) : llm.model;
    child.max_iterations = max_iterations;
    child.quiet_mode = true;
    child.registry = &reg;
    if (!assigned) child.llm = &llm;
    child.gate = deps.parent ? deps.parent->gate : nullptr;   // the parent's scope holds for children
    child.delegate_depth = child_depth;
    child.ephemeral_system_prompt = delegate_child_prompt(goal, context, deps.workspace_hint);

    try {
        const std::string summary = child.run(goal);
        const double dur = std::chrono::duration<double>(clock::now() - t0).count();
        const bool interrupted = interrupt::current() && interrupt::is_interrupted();
        return child_entry(task_index,
                           interrupted ? "interrupted" : (summary.empty() ? "failed" : "completed"),
                           summary, child.api_call_count, dur, child.model,
                           interrupted ? "interrupted" : "completed",
                           prompt_tokens.load(), completion_tokens.load(),
                           summary.empty() && !interrupted ? "Subagent did not produce a response." : "");
    } catch (const std::exception& ex) {
        const double dur = std::chrono::duration<double>(clock::now() - t0).count();
        const bool interrupted = interrupt::current() && interrupt::is_interrupted();
        return child_entry(task_index, interrupted ? "interrupted" : "error", "", child.api_call_count,
                           dur, child.model, interrupted ? "interrupted" : "error",
                           prompt_tokens.load(), completion_tokens.load(),
                           interrupted ? "interrupted by the operator" : ex.what());
    }
}

std::string str_arg(const Json& o, const char* key) {
    const Json* v = json_get(o, key);
    return v && v->type == Json::Str ? v->str : std::string();
}

}  // namespace

std::string delegate_child_prompt(const std::string& goal, const std::string& context,
                                  const std::string& workspace_path) {
    std::string out =
        "You are a focused subagent working on a specific delegated task.\n"
        "\n"
        "YOUR TASK:\n" + goal;
    if (!context.empty()) out += "\n\nCONTEXT:\n" + context;
    if (!workspace_path.empty())
        out += "\n\nWORKSPACE PATH:\n" + workspace_path + "\n"
               "Use this exact path for local repository/workdir operations unless the task explicitly says otherwise.";
    out +=
        "\n\nComplete this task using the tools available to you. "
        "When finished, provide a clear, concise summary of:\n"
        "- What you did\n"
        "- What you found or accomplished\n"
        "- Any files you created or modified\n"
        "- Any issues encountered\n"
        "\n"
        "Important workspace rule: Never assume a repository lives at /workspace/... or any other "
        "container-style path unless the task/context explicitly gives that path. If no exact local "
        "path is provided, discover it first before issuing git/workdir-specific commands.\n"
        "\n"
        "Be thorough but concise -- your response is returned to the parent agent as a summary.";
    return out;
}

Json delegate_tool_schema() {
    auto str_prop = [](const std::string& desc) {
        return json_obj({{"type", jsp(json_str("string"))}, {"description", jsp(json_str(desc))}});
    };
    Json toolset_names = json_arr();
    arr_push(toolset_names, json_str("file"));
    arr_push(toolset_names, json_str("terminal"));

    Json goal = str_prop("the task for one sub-agent");
    Json context = str_prop("background the sub-agent needs (optional)");
    Json toolsets = json_obj();
    obj_set(toolsets, "type", json_str("array"));
    {
        Json items = json_obj();
        obj_set(items, "type", json_str("string"));
        obj_set(items, "enum", toolset_names);
        obj_set(toolsets, "items", items);
    }
    obj_set(toolsets, "description", json_str("which toolsets the sub-agent gets; omit for all"));

    Json task_props = json_obj();
    obj_set(task_props, "goal", goal);
    obj_set(task_props, "context", context);
    obj_set(task_props, "toolsets", toolsets);
    Json task = json_obj();
    obj_set(task, "type", json_str("object"));
    obj_set(task, "properties", task_props);
    obj_set(task, "required", json_arr({jsp(json_str("goal"))}));

    Json props = json_obj();
    obj_set(props, "goal", str_prop("single-task mode: the task for one sub-agent"));
    obj_set(props, "context", str_prop("single-task mode: background the sub-agent needs"));
    {
        Json t = json_obj();
        obj_set(t, "type", json_str("array"));
        obj_set(t, "items", json_obj({{"type", jsp(json_str("string"))}}));
        obj_set(t, "description", json_str("which toolsets the sub-agent(s) get; omit for all"));
        obj_set(props, "toolsets", t);
    }
    {
        Json tasks = json_obj();
        obj_set(tasks, "type", json_str("array"));
        obj_set(tasks, "items", task);
        obj_set(tasks, "description",
                json_str("batch mode: up to 3 tasks, run concurrently; results come back in task order"));
        obj_set(props, "tasks", tasks);
    }
    obj_set(props, "max_iterations", json_obj({{"type", jsp(json_str("integer"))},
                                               {"description", jsp(json_str("LLM calls a sub-agent may make (default 50)"))}}));

    Json params = json_obj();
    obj_set(params, "type", json_str("object"));
    obj_set(params, "properties", props);

    Json schema = json_obj();
    obj_set(schema, "description", json_str(
        "Spawn one or more focused sub-agents. One task: goal (+context). Several: tasks=[{goal,context},...] "
        "up to 3, run concurrently. Each sub-agent is a fresh agent loop with its own transcript and its own "
        "todo ledger, holding the file and terminal tools; it cannot see this conversation and reports back a "
        "summary only. Delegation ends " + std::to_string(kDelegateMaxDepth) + " levels deep. Returns JSON: "
        "results[{task_index,status,summary,api_calls,duration_seconds,exit_reason,tokens}], total_duration_seconds."));
    obj_set(schema, "parameters", params);
    return schema;
}

std::function<std::string(const Json&)> make_delegate_tool(const DelegateToolDeps& deps) {
    return [deps](const Json& args) -> std::string {
        if (!deps.parent) return tool_error("delegate_task requires a parent agent context.");
        const int depth = deps.parent->delegate_depth;
        if (depth >= kDelegateMaxDepth)
            return tool_error("Delegation depth limit reached (" + std::to_string(kDelegateMaxDepth) +
                              "). Subagents cannot spawn further subagents.");

        int max_iterations = kDelegateMaxIterations;
        if (const Json* mi = json_get(args, "max_iterations"); mi && mi->type == Json::Num && mi->num > 0)
            max_iterations = static_cast<int>(mi->num);

        // Which toolsets the children keep: requested ∩ what a child has, with
        // "delegation" stripped (delegate_tool.py::_strip_blocked_tools); the
        // depth rule above decides delegate_task on its own.
        std::set<std::string> wanted;
        if (const Json* ts = json_get(args, "toolsets"); ts && ts->type == Json::Arr)
            for (const auto& sp : ts->arr)
                if (sp && sp->type == Json::Str && sp->str != "delegation") wanted.insert(sp->str);

        // Task list: batch (capped) or single.
        struct Task { std::string goal, context; };
        std::vector<Task> tasks;
        if (const Json* tl = json_get(args, "tasks"); tl && tl->type == Json::Arr && !tl->arr.empty()) {
            for (const auto& sp : tl->arr) {
                if (!sp || sp->type != Json::Obj) return tool_error("each task must be an object with a 'goal'.");
                Task t{str_arg(*sp, "goal"), str_arg(*sp, "context")};
                const Json* sp_ts = json_get(*sp, "toolsets");
                if (sp_ts && sp_ts->type == Json::Arr)
                    for (const auto& s2 : sp_ts->arr)
                        if (s2 && s2->type == Json::Str && s2->str != "delegation") wanted.insert(s2->str);
                tasks.push_back(std::move(t));
            }
            if (tasks.size() > static_cast<size_t>(kDelegateMaxChildren))
                tasks.resize(static_cast<size_t>(kDelegateMaxChildren));
        } else {
            const std::string goal = str_arg(args, "goal");
            if (goal.empty()) return tool_error("Provide either 'goal' (single task) or 'tasks' (batch).");
            tasks.push_back({goal, str_arg(args, "context")});
        }
        for (size_t i = 0; i < tasks.size(); ++i)
            if (tasks[i].goal.empty())
                return tool_error("Task " + std::to_string(i) + " is missing a 'goal'.");

        const int child_depth = depth + 1;
        const auto started = std::chrono::steady_clock::now();
        std::vector<Json> results(tasks.size());
        // One transport per child, assigned here — on the handler's thread, before
        // any child thread starts. A factory that is itself thread-unsafe cannot be
        // raced by this tool; the pool hands out, the threads only run.
        std::vector<std::shared_ptr<agent::LLMClient>> assigned;
        if (deps.llm_factory) {
            assigned.reserve(tasks.size());
            for (size_t i = 0; i < tasks.size(); ++i) assigned.push_back(deps.llm_factory());
        }

        if (tasks.size() == 1) {
            results[0] = run_one_child(0, tasks[0].goal, tasks[0].context, child_depth,
                                       max_iterations, deps, wanted,
                                       assigned.empty() ? nullptr : assigned[0]);
        } else {
            std::vector<std::thread> workers;
            workers.reserve(tasks.size());
            for (size_t i = 0; i < tasks.size(); ++i)
                workers.emplace_back([&, i] {
                    results[i] = run_one_child(static_cast<int>(i), tasks[i].goal, tasks[i].context,
                                               child_depth, max_iterations, deps, wanted,
                                               assigned.empty() ? nullptr : assigned[i]);
                });
            for (auto& t : workers) t.join();
        }
        const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

        Json arr = json_arr();
        for (auto& r : results) arr_push(arr, r);
        Json out = json_obj({{"results", jsp(std::move(arr))},
                             {"total_duration_seconds", jsp(json_num(std::round(total * 100.0) / 100.0))}});
        return json_dump(out);
    };
}

}  // namespace tools
}  // namespace dd
