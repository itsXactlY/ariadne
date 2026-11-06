// tests/delegate_tool_test.cpp — delegate_task: depth and width limits, the child
// prompt and toolset, batch order, and the report the parent's model reads.
#include <algorithm>
#include <atomic>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "dd/agent/agent.hpp"
#include "dd/core/json.hpp"
#include "dd/core/message.hpp"
#include "dd/tools/delegate_tool.hpp"
#include "dd/tools/registry.hpp"

using namespace dd;
using core::Json;
using core::JsonSP;
using core::arr_push;
using core::json_arr;
using core::json_get;
using core::json_num;
using core::json_obj;
using core::json_parse;
using core::json_str;

static int g_fail = 0, g_pass = 0;
static void chk(bool ok, const std::string& what) {
    if (ok) ++g_pass;
    else {
        ++g_fail;
        std::cout << "FAIL: " << what << "\n";
    }
}

static JsonSP jsp(Json v) { return std::make_shared<Json>(std::move(v)); }

// A scripted child transport: each entry is one assistant message; past the end it
// answers "done". Keeps every request and the tool list it was offered.
struct Script : agent::LLMClient {
    std::vector<core::Message> replies;
    bool die = false;   // every call throws
    std::vector<std::vector<core::Message>> requests;
    std::vector<Json> tools_seen;
    int calls = 0;

    core::Message chat(const std::vector<core::Message>& m, const Json& tools) override {
        ++calls;
        requests.push_back(m);
        tools_seen.push_back(tools);
        if (die) throw std::runtime_error("wire down");
        core::Message a;
        a.role = "assistant";
        if (static_cast<size_t>(calls) <= replies.size()) return replies[static_cast<size_t>(calls) - 1];
        a.content = "done";
        a.finish_reason = "stop";
        return a;
    }
};

static core::ToolCall call(const std::string& id, const std::string& name, const std::string& args) {
    core::ToolCall c;
    c.id = id;
    c.name = name;
    c.arguments = args;
    return c;
}

static core::Message text_reply(const std::string& text) {
    core::Message m;
    m.role = "assistant";
    m.content = text;
    m.finish_reason = "stop";
    return m;
}

static core::Message tool_reply(const std::string& id, const std::string& name, const std::string& args) {
    core::Message m;
    m.role = "assistant";
    m.tool_calls = {call(id, name, args)};
    return m;
}

// The tool names one Script was offered, across all of its calls.
static std::vector<std::string> tool_names(const Script& s) {
    std::vector<std::string> names;
    for (const auto& tools : s.tools_seen)
        for (const auto& def : tools.arr)
            if (def && def->type == Json::Obj)
                if (const Json* fn = json_get(*def, "function"))
                    if (const Json* n = json_get(*fn, "name"))
                        if (std::find(names.begin(), names.end(), n->str) == names.end())
                            names.push_back(n->str);
    return names;
}

static bool has_tool(const Script& s, const std::string& name) {
    const auto names = tool_names(s);
    return std::find(names.begin(), names.end(), name) != names.end();
}

// A fresh Script per child, popped from a queue in creation order (single-task
// runs are sequential, so child-before-grandchild is deterministic).
struct ScriptPool {
    std::vector<std::shared_ptr<Script>> scripts;
    size_t next = 0;
    std::shared_ptr<agent::LLMClient> make() {
        if (next >= scripts.size()) scripts.push_back(std::make_shared<Script>());
        return scripts[next++];
    }
};

static Json run_delegate(agent::Agent& parent, ScriptPool& pool, const Json& args) {
    tools::DelegateToolDeps deps;
    deps.parent = &parent;
    deps.llm_factory = [&pool] { return pool.make(); };
    const auto handler = tools::make_delegate_tool(deps);
    return json_parse(handler(args));
}

int main() {
    // ---- the ceiling: a parent at MAX_DEPTH is refused, in so many words ----
    {
        agent::Agent parent;
        parent.delegate_depth = tools::kDelegateMaxDepth;
        tools::DelegateToolDeps deps;
        deps.parent = &parent;
        const auto handler = tools::make_delegate_tool(deps);
        const std::string out = handler(json_obj({{"goal", jsp(json_str("x"))}}));
        chk(out.find("Delegation depth limit reached (" + std::to_string(tools::kDelegateMaxDepth)) != std::string::npos,
            "depth cap names the ceiling");
    }

    // ---- the argument contract ----
    {
        agent::Agent parent;
        ScriptPool pool;
        const Json out = run_delegate(parent, pool, json_obj({{"context", jsp(json_str("no goal"))}}));
        const Json* e = json_get(out, "error");
        chk(e && e->type == Json::Str && e->str.find("Provide either 'goal'") != std::string::npos,
            "no goal and no tasks is refused");
    }
    {
        agent::Agent parent;
        ScriptPool pool;
        Json tasks = json_arr();
        arr_push(tasks, json_obj({{"context", jsp(json_str("missing goal"))}}));
        const Json out = run_delegate(parent, pool, json_obj({{"tasks", jsp(std::move(tasks))}}));
        const Json* e = json_get(out, "error");
        chk(e && e->type == Json::Str && e->str.find("Task 0 is missing a 'goal'") != std::string::npos,
            "a task without a goal names its index");
    }

    // ---- one task: the child runs, the summary rides back ----
    {
        agent::Agent parent;
        ScriptPool pool;
        auto s = std::make_shared<Script>();
        s->replies = {text_reply("child done")};
        pool.scripts.push_back(s);
        const Json out = run_delegate(parent, pool, json_obj({{"goal", jsp(json_str("do the thing"))}}));
        const Json* rs = json_get(out, "results");
        chk(rs && rs->type == Json::Arr && rs->arr.size() == 1, "one task, one result");
        const Json& r = *rs->arr[0];
        const Json* st = json_get(r, "status");
        chk(st && st->str == "completed", "status completed");
        const Json* sum = json_get(r, "summary");
        chk(sum && sum->type == Json::Str && sum->str == "child done", "summary is the child's last words");
        const Json* ac = json_get(r, "api_calls");
        chk(ac && ac->num == 1, "api_calls 1");
        const Json* xr = json_get(r, "exit_reason");
        chk(xr && xr->str == "completed", "exit_reason completed");
        const Json* ti = json_get(r, "task_index");
        chk(ti && ti->num == 0, "task_index 0");
        chk(json_get(out, "total_duration_seconds") != nullptr, "total_duration_seconds present");
        chk(s->requests.size() == 1 && s->requests[0][0].content.find("YOUR TASK:\ndo the thing") != std::string::npos,
            "the child's first message is the focused prompt");
        chk(s->requests[0][0].role == "user", "the child's prompt goes out as a user turn");
        // A depth-0 parent's child may still delegate (depth 1 < 2).
        chk(has_tool(*s, "read_file") && has_tool(*s, "write_file") && has_tool(*s, "list_dir") &&
            has_tool(*s, "edit_file") && has_tool(*s, "run_terminal") && has_tool(*s, "job") &&
            has_tool(*s, "todo"), "the child holds the file and terminal tools plus its own todo");
        chk(has_tool(*s, "delegate_task"), "a depth-1 child may delegate on");
    }

    // ---- the tree ends: a child at MAX_DEPTH gets no delegate_task ----
    {
        agent::Agent parent;
        parent.delegate_depth = tools::kDelegateMaxDepth - 1;
        ScriptPool pool;
        auto s = std::make_shared<Script>();
        s->replies = {text_reply("leaf work done")};
        pool.scripts.push_back(s);
        const Json out = run_delegate(parent, pool, json_obj({{"goal", jsp(json_str("leaf"))}}));
        const Json* rs = json_get(out, "results");
        const Json* st = json_get(*rs->arr[0], "status");
        chk(st && st->str == "completed", "a depth-1 parent still spawns");
        chk(!has_tool(*s, "delegate_task"), "a child at MAX_DEPTH holds no delegate_task");
        chk(has_tool(*s, "read_file"), "the leaf child still holds the file tools");
    }

    // ---- two hops: the child delegates, the grandchild's work comes home ----
    {
        agent::Agent parent;
        ScriptPool pool;
        auto child = std::make_shared<Script>();
        child->replies = {tool_reply("d1", "delegate_task", "{\"goal\":\"leaf task\"}"),
                          text_reply("assembled the leaf's answer")};
        auto leaf = std::make_shared<Script>();
        leaf->replies = {text_reply("leaf done")};
        pool.scripts = {child, leaf};
        const Json out = run_delegate(parent, pool, json_obj({{"goal", jsp(json_str("two hops"))}}));
        const Json* rs = json_get(out, "results");
        const Json* sum = json_get(*rs->arr[0], "summary");
        chk(sum && sum->str == "assembled the leaf's answer", "the child's summary rides back");
        const Json* ac = json_get(*rs->arr[0], "api_calls");
        chk(ac && ac->num == 2, "the child made 2 calls: delegate + wrap-up");
        chk(leaf->requests.size() == 1 &&
            leaf->requests[0][0].content.find("YOUR TASK:\nleaf task") != std::string::npos,
            "the grandchild ran on its own focused prompt");
        chk(!has_tool(*leaf, "delegate_task"),
            "the grandchild at MAX_DEPTH holds no delegate_task (the tree ends there)");
        // the child saw its own child's report as a tool result
        bool saw_leaf_report = false;
        for (const auto& m : child->requests[1])
            if (m.role == "tool" && m.content.find("leaf done") != std::string::npos) saw_leaf_report = true;
        chk(saw_leaf_report, "the grandchild's summary reached the child as its tool result");
    }

    // ---- batch: one call, several children, results in task order ----
    {
        agent::Agent parent;
        ScriptPool pool;
        for (const char* w : {"alpha", "beta"}) {
            auto s = std::make_shared<Script>();
            s->replies = {text_reply(w)};
            pool.scripts.push_back(s);
        }
        Json tasks = json_arr();
        arr_push(tasks, json_obj({{"goal", jsp(json_str("A"))}}));
        arr_push(tasks, json_obj({{"goal", jsp(json_str("B"))}}));
        const Json out = run_delegate(parent, pool, json_obj({{"tasks", jsp(std::move(tasks))}}));
        const Json* rs = json_get(out, "results");
        chk(rs && rs->type == Json::Arr && rs->arr.size() == 2, "two tasks, two results");
        const Json* i0 = json_get(*rs->arr[0], "task_index");
        const Json* i1 = json_get(*rs->arr[1], "task_index");
        chk(i0 && i1 && i0->num == 0 && i1->num == 1, "results come back in task order");
        const Json* s0 = json_get(*rs->arr[0], "status");
        const Json* s1 = json_get(*rs->arr[1], "status");
        chk(s0 && s1 && s0->str == "completed" && s1->str == "completed", "both children completed");
        const Json* sum0 = json_get(*rs->arr[0], "summary");
        const Json* sum1 = json_get(*rs->arr[1], "summary");
        const bool pair_ok = (sum0->str == "alpha" && sum1->str == "beta") ||
                             (sum0->str == "beta" && sum1->str == "alpha");
        chk(pair_ok, "each child's own summary sits in its own entry");
    }

    // ---- the width cap: extra tasks are dropped, not run ----
    {
        agent::Agent parent;
        ScriptPool pool;
        Json tasks = json_arr();
        for (int i = 0; i < 5; ++i) arr_push(tasks, json_obj({{"goal", jsp(json_str("t" + std::to_string(i)))}}));
        const Json out = run_delegate(parent, pool, json_obj({{"tasks", jsp(std::move(tasks))}}));
        const Json* rs = json_get(out, "results");
        chk(rs && rs->arr.size() == 3, "batch caps at kDelegateMaxChildren");
        chk(pool.scripts.size() == 3, "no child beyond the cap was ever built");
    }

    // ---- toolsets: the child gets the named set, nothing else ----
    {
        agent::Agent parent;
        ScriptPool pool;
        auto s = std::make_shared<Script>();
        s->replies = {text_reply("ok")};
        pool.scripts.push_back(s);
        Json ts = json_arr();
        arr_push(ts, json_str("terminal"));
        Json args = json_obj({{"goal", jsp(json_str("term only"))}, {"toolsets", jsp(std::move(ts))}});
        const Json out = run_delegate(parent, pool, args);
        const Json* rs = json_get(out, "results");
        const Json* st = json_get(*rs->arr[0], "status");
        chk(st && st->str == "completed", "a filtered child still completes");
        chk(has_tool(*s, "run_terminal") && has_tool(*s, "job"), "terminal toolset present");
        chk(!has_tool(*s, "read_file") && !has_tool(*s, "write_file"), "file tools filtered away");
        chk(has_tool(*s, "todo"), "the todo ledger is never filtered away");
        chk(has_tool(*s, "delegate_task"), "delegation is never filtered away");
    }

    // ---- a child's failure is a result entry, not a throw ----
    {
        agent::Agent parent;
        ScriptPool pool;
        auto s = std::make_shared<Script>();
        s->die = true;
        pool.scripts.push_back(s);
        const Json out = run_delegate(parent, pool, json_obj({{"goal", jsp(json_str("doomed"))}}));
        const Json* rs = json_get(out, "results");
        const Json* st = json_get(*rs->arr[0], "status");
        chk(st && st->str == "error", "a throwing child reports error");
        const Json* er = json_get(*rs->arr[0], "error");
        chk(er && er->type == Json::Str && er->str.find("wire down") != std::string::npos,
            "the error names what happened");
        const Json* xr = json_get(*rs->arr[0], "exit_reason");
        chk(xr && xr->str == "error", "exit_reason error");
    }

    // ---- a blank child reply is the loop's business, not the tool's: it retries ----
    // (Agent::run never returns an empty final response — a blank reply is retried
    // and closed with the loop's epilogue — so the "failed" status exists only for
    // a run() that somehow answers with nothing.)
    {
        agent::Agent parent;
        ScriptPool pool;
        auto s = std::make_shared<Script>();
        s->replies = {text_reply("")};
        pool.scripts.push_back(s);
        const Json out = run_delegate(parent, pool, json_obj({{"goal", jsp(json_str("silent"))}}));
        const Json* rs = json_get(out, "results");
        const Json* st = json_get(*rs->arr[0], "status");
        chk(st && st->str == "completed", "a blank reply is retried by the child's own loop");
        const Json* ac = json_get(*rs->arr[0], "api_calls");
        chk(ac && ac->num >= 2, "the blank reply cost more than one call");
        const Json* sum = json_get(*rs->arr[0], "summary");
        chk(sum && sum->type == Json::Str && !sum->str.empty(), "the epilogue is the summary");
    }

    std::cout << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail ? 1 : 0;
}
