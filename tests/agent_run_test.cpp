// tests/agent_run_test.cpp — the run loop's exits: interrupts inside a batch, dedup before recording, end_turn on every path.
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "dd/agent/agent.hpp"
#include "dd/agent/iteration_budget.hpp"
#include "dd/core/json.hpp"
#include "dd/core/message.hpp"
#include "dd/tools/registry.hpp"
#include "dd/tools/tool.hpp"

using namespace dd;
using core::Json;

static int g_fail = 0, g_pass = 0;
static void chk(bool ok, const std::string& what) {
    if (ok) ++g_pass;
    else {
        ++g_fail;
        std::cout << "FAIL: " << what << "\n";
    }
}

static core::ToolCall call(const std::string& id, const std::string& name, const std::string& args) {
    core::ToolCall c;
    c.id = id;
    c.name = name;
    c.arguments = args;
    return c;
}

// Answers from a script: each entry is one assistant message; past the end it answers "done". Keeps every request.
struct Script : agent::LLMClient {
    std::vector<core::Message> replies;
    std::vector<std::vector<core::Message>> requests;
    int calls = 0;
    bool throw_interrupted = false;
    bool always_tools = false;   // every answer, a summary request included, is a tool call
    core::Message chat(const std::vector<core::Message>& m, const Json&) override {
        ++calls;
        requests.push_back(m);
        if (throw_interrupted) throw std::runtime_error("interrupted");
        core::Message a;
        a.role = "assistant";
        if (always_tools) {
            a.tool_calls = {call("t" + std::to_string(calls), "echo", "{}")};
            return a;
        }
        if (static_cast<size_t>(calls) <= replies.size()) return replies[static_cast<size_t>(calls) - 1];
        a.content = "done";
        a.finish_reason = "stop";
        return a;
    }
};

struct Spy : agent::ContextEngine {
    int begun = 0, ended = 0, ended_interrupted = 0;
    std::string last_final = "(unset)";
    bool throw_on_end = false;
    std::vector<size_t> assistant_calls, result_counts;
    void begin_turn(const std::string&) override { ++begun; }
    std::vector<core::Message> payload() override {
        core::Message u;
        u.role = "user";
        u.content = "go";
        return {u};
    }
    core::Json tools() override { return core::json_arr(); }
    void on_assistant(const core::Message& m) override { assistant_calls.push_back(m.tool_calls.size()); }
    void on_tool_results(const std::vector<core::Message>& r) override { result_counts.push_back(r.size()); }
    void after_tools() override {}
    void end_turn(const std::string& f, bool interrupted) override {
        ++ended;
        ended_interrupted += interrupted ? 1 : 0;
        last_final = f;
        if (throw_on_end) throw std::runtime_error("end_turn failed");
    }
};

static void add_tool(tools::ToolRegistry& reg, const std::string& name, std::function<std::string(const Json&)> h) {
    tools::ToolEntry e;
    e.name = name;
    e.handler = std::move(h);
    reg.register_tool(e);
}

static bool cancelled_text(const std::string& s) { return s.find("cancelled") != std::string::npos && s.find("user interrupt") != std::string::npos; }

// Collects what step_hook saw of the tool results: (id, content), in order.
struct Seen {
    std::vector<std::pair<std::string, std::string>> tool;
    void attach(agent::Agent& a) {
        a.step_hook = [this](const core::Message& m) {
            if (m.role == "tool") tool.emplace_back(m.tool_call_id, m.content);
        };
    }
    int cancelled() const {
        int n = 0;
        for (const auto& [id, c] : tool) n += cancelled_text(c) ? 1 : 0;
        return n;
    }
    bool one_each(const std::vector<std::string>& ids) const {
        if (tool.size() != ids.size()) return false;
        for (const auto& id : ids) {
            int k = 0;
            for (const auto& [tid, c] : tool) k += tid == id ? 1 : 0;
            if (k != 1) return false;
        }
        return true;
    }
};

// (a) an interrupt set inside the first of four sequential calls: that call finishes, the rest are cancelled unrun
static void test_interrupt_sequential() {
    tools::ToolRegistry reg;
    agent::Agent ag;
    std::atomic<int> ran{0};
    add_tool(reg, "echo", [&](const Json&) {
        ++ran;
        ag.interrupt_requested = true;
        return std::string("{\"ok\":true}");
    });
    Script llm;
    core::Message m;
    m.role = "assistant";
    m.tool_calls = {call("c1", "echo", "{\"x\":1}"), call("c2", "echo", "{\"x\":2}"), call("c3", "echo", "{\"x\":3}"), call("c4", "echo", "{\"x\":4}")};
    llm.replies = {m};
    Spy ctx;
    Seen seen;
    seen.attach(ag);
    ag.registry = &reg;
    ag.llm = &llm;
    ag.context = &ctx;
    const std::string final = ag.run("go");
    chk(final.empty(), "sequential interrupt: the turn ends without an answer");
    chk(ran == 1, "sequential interrupt: only the running call ran, ran=" + std::to_string(ran.load()));
    chk(seen.one_each({"c1", "c2", "c3", "c4"}), "sequential interrupt: every tool_call id has exactly one result");
    chk(seen.cancelled() == 3, "sequential interrupt: the other three are cancelled, cancelled=" + std::to_string(seen.cancelled()));
    chk(!seen.tool.empty() && seen.tool[0].second == "{\"ok\":true}", "sequential interrupt: the running call keeps its real result");
    chk(llm.calls == 1, "sequential interrupt: no further LLM call");
    chk(ctx.ended == 1 && ctx.ended_interrupted == 1, "sequential interrupt: end_turn once, interrupted");
    chk(ctx.result_counts.size() == 1 && ctx.result_counts[0] == 4, "sequential interrupt: the engine is told all four results");
}

// (a) the parallel path: ten calls are two batches (8 workers); an interrupt set by the first batch cancels the second
static void test_interrupt_parallel() {
    tools::ToolRegistry reg;
    agent::Agent ag;
    std::atomic<int> ran{0};
    add_tool(reg, "read_file", [&](const Json&) {
        ++ran;
        ag.interrupt_requested = true;
        return std::string("{\"content\":\"x\"}");
    });
    Script llm;
    core::Message m;
    m.role = "assistant";
    std::vector<std::string> ids;
    for (int i = 0; i < 10; ++i) {
        ids.push_back("p" + std::to_string(i));
        m.tool_calls.push_back(call(ids.back(), "read_file", "{\"path\":\"/dir/f" + std::to_string(i) + "\"}"));
    }
    llm.replies = {m};
    Seen seen;
    seen.attach(ag);
    ag.registry = &reg;
    ag.llm = &llm;
    chk(agent::should_parallelize(m.tool_calls, nullptr), "parallel interrupt: the batch is parallel");
    ag.run("go");
    chk(seen.one_each(ids), "parallel interrupt: every id has exactly one result");
    chk(ran >= 1 && ran <= 8, "parallel interrupt: only the first batch ran, ran=" + std::to_string(ran.load()));
    chk(seen.cancelled() == 10 - ran, "parallel interrupt: everything that did not run is cancelled");
    chk(seen.tool.size() == 10 && cancelled_text(seen.tool[8].second) && cancelled_text(seen.tool[9].second),
        "parallel interrupt: the second batch never started");
}

// an interrupt already set when the batch begins (a gate sets it): nothing runs, in both paths
struct InterruptingGate : agent::ToolGate {
    agent::Agent* ag = nullptr;
    std::optional<std::string> veto(const core::ToolCall&) override {
        ag->interrupt_requested = true;
        return std::nullopt;
    }
};

static void test_interrupt_before_batch() {
    for (const bool parallel : {false, true}) {
        tools::ToolRegistry reg;
        agent::Agent ag;
        std::atomic<int> ran{0};
        const std::string name = parallel ? "read_file" : "echo";
        add_tool(reg, name, [&](const Json&) {
            ++ran;
            return std::string("{}");
        });
        Script llm;
        core::Message m;
        m.role = "assistant";
        for (int i = 0; i < 4; ++i)
            m.tool_calls.push_back(call("g" + std::to_string(i), name, parallel ? "{\"path\":\"/d/f" + std::to_string(i) + "\"}" : "{\"x\":" + std::to_string(i) + "}"));
        llm.replies = {m};
        InterruptingGate gate;
        gate.ag = &ag;
        Seen seen;
        seen.attach(ag);
        ag.registry = &reg;
        ag.llm = &llm;
        ag.gate = &gate;
        ag.run("go");
        const std::string label = parallel ? "parallel" : "sequential";
        chk(ran == 0, label + " interrupt before the batch: no handler ran");
        chk(seen.one_each({"g0", "g1", "g2", "g3"}) && seen.cancelled() == 4, label + " interrupt before the batch: four cancelled results");
    }
}

// (b) no hidden pause between sequential calls; (b2) when a delay is asked for, an interrupt ends it at once
static void test_tool_delay() {
    {
        agent::Agent probe;
        chk(probe.tool_delay == 0.0, "tool_delay defaults to 0");
    }
    {
        tools::ToolRegistry reg;
        add_tool(reg, "echo", [](const Json&) { return std::string("{}"); });
        Script llm;
        core::Message m;
        m.role = "assistant";
        m.tool_calls = {call("a", "echo", "{\"x\":1}"), call("b", "echo", "{\"x\":2}"), call("c", "echo", "{\"x\":3}")};
        llm.replies = {m};
        agent::Agent ag;
        ag.registry = &reg;
        ag.llm = &llm;
        const auto t0 = std::chrono::steady_clock::now();
        const std::string final = ag.run("go");
        const long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        chk(final == "done" && ms < 100, "three sequential calls take < 100 ms by default, took " + std::to_string(ms));
    }
    {
        tools::ToolRegistry reg;
        agent::Agent ag;
        add_tool(reg, "echo", [&](const Json&) {
            ag.interrupt_requested = true;
            return std::string("{}");
        });
        Script llm;
        core::Message m;
        m.role = "assistant";
        m.tool_calls = {call("a", "echo", "{\"x\":1}"), call("b", "echo", "{\"x\":2}"), call("c", "echo", "{\"x\":3}")};
        llm.replies = {m};
        ag.registry = &reg;
        ag.llm = &llm;
        ag.tool_delay = 5.0;
        Seen seen;
        seen.attach(ag);
        const auto t0 = std::chrono::steady_clock::now();
        ag.run("go");
        const long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        chk(ms < 1000, "an interrupt ends the tool_delay wait, took " + std::to_string(ms) + " ms");
        chk(seen.cancelled() == 2, "after the interrupted wait the remaining calls are cancelled");
    }
    {   // a delay that nobody interrupts is honoured
        tools::ToolRegistry reg;
        add_tool(reg, "echo", [](const Json&) { return std::string("{}"); });
        Script llm;
        core::Message m;
        m.role = "assistant";
        m.tool_calls = {call("a", "echo", "{\"x\":1}"), call("b", "echo", "{\"x\":2}")};
        llm.replies = {m};
        agent::Agent ag;
        ag.registry = &reg;
        ag.llm = &llm;
        ag.tool_delay = 0.2;
        const auto t0 = std::chrono::steady_clock::now();
        ag.run("go");
        const long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        chk(ms >= 190, "an asked-for tool_delay is honoured, took " + std::to_string(ms) + " ms");
    }
}

// (c) a duplicate call leaves the message: the next request has as many tool results as tool_calls
static void test_dedup_before_recording() {
    tools::ToolRegistry reg;
    std::atomic<int> ran{0};
    add_tool(reg, "echo", [&](const Json&) {
        ++ran;
        return std::string("{}");
    });
    Script llm;
    core::Message m;
    m.role = "assistant";
    m.tool_calls = {call("c1", "echo", "{\"x\":1}"), call("c2", "echo", "{\"x\":1}"), call("c3", "echo", "{\"x\":2}")};
    llm.replies = {m};
    Spy ctx;
    agent::Agent ag;
    ag.registry = &reg;
    ag.llm = &llm;
    ag.context = &ctx;
    Seen seen;
    seen.attach(ag);
    ag.run("go");
    chk(ran == 2, "dedup: the duplicate does not run");
    chk(ctx.assistant_calls.size() >= 1 && ctx.assistant_calls[0] == 2, "dedup: the engine records the message without the duplicate");
    chk(ctx.result_counts.size() == 1 && ctx.result_counts[0] == 2, "dedup: the engine gets two results");
    chk(seen.one_each({"c1", "c3"}), "dedup: results for c1 and c3 only");

    // without an engine the loop's own transcript is what the next request carries
    Script llm2;
    llm2.replies = {m};
    tools::ToolRegistry reg2;
    add_tool(reg2, "echo", [](const Json&) { return std::string("{}"); });
    agent::Agent ag2;
    ag2.registry = &reg2;
    ag2.llm = &llm2;
    ag2.run("go");
    chk(llm2.requests.size() == 2, "dedup: two requests");
    if (llm2.requests.size() == 2) {
        size_t calls_n = 0, tool_n = 0;
        for (const auto& msg : llm2.requests[1]) {
            if (msg.role == "assistant") calls_n += msg.tool_calls.size();
            if (msg.role == "tool") ++tool_n;
        }
        chk(calls_n == 2 && tool_n == 2, "dedup: next request has equal tool_calls and tool results (" + std::to_string(calls_n) + "/" + std::to_string(tool_n) + ")");
    }

    // delegate calls beyond three leave the message the same way
    Script llm3;
    core::Message d;
    d.role = "assistant";
    for (int i = 0; i < 5; ++i) d.tool_calls.push_back(call("d" + std::to_string(i), "delegate_task", "{\"i\":" + std::to_string(i) + "}"));
    d.tool_calls.push_back(call("e", "echo", "{}"));
    llm3.replies = {d};
    Spy ctx3;
    agent::Agent ag3;
    ag3.registry = &reg2;
    ag3.llm = &llm3;
    ag3.context = &ctx3;
    ag3.run("go");
    chk(ctx3.assistant_calls.size() >= 1 && ctx3.assistant_calls[0] == 4 && ctx3.result_counts.size() == 1 && ctx3.result_counts[0] == 4,
        "cap: three delegate calls and the echo, four results");
}

// (d) every way out closes the engine's turn once
static void test_end_turn() {
    {   // an LLM call that throws (Esc during generation)
        Script llm;
        llm.throw_interrupted = true;
        Spy ctx;
        agent::Agent ag;
        ag.llm = &llm;
        ag.context = &ctx;
        std::string what;
        try {
            ag.run("go");
        } catch (const std::exception& e) {
            what = e.what();
        }
        chk(what == "interrupted", "throwing llm: the exception propagates unchanged, got '" + what + "'");
        chk(ctx.begun == 1 && ctx.ended == 1 && ctx.ended_interrupted == 1 && ctx.last_final.empty(), "throwing llm: end_turn once, interrupted, no answer");
    }
    {   // end_turn itself throws while an exception is in flight: the original one wins, end_turn is not retried
        Script llm;
        llm.throw_interrupted = true;
        Spy ctx;
        ctx.throw_on_end = true;
        agent::Agent ag;
        ag.llm = &llm;
        ag.context = &ctx;
        std::string what;
        try {
            ag.run("go");
        } catch (const std::exception& e) {
            what = e.what();
        }
        chk(what == "interrupted" && ctx.ended == 1, "throwing end_turn: the original error propagates, end_turn once");
    }
    {   // a normal finish
        Script llm;
        Spy ctx;
        agent::Agent ag;
        ag.llm = &llm;
        ag.context = &ctx;
        const std::string final = ag.run("go");
        chk(final == "done" && ctx.begun == 1 && ctx.ended == 1 && ctx.ended_interrupted == 0 && ctx.last_final == "done",
            "normal run: end_turn once with the answer, not interrupted");
    }
    {   // the flag was already set at the top of the loop
        Script llm;
        Spy ctx;
        agent::Agent ag;
        ag.llm = &llm;
        ag.context = &ctx;
        ag.interrupt_requested = true;
        const std::string final = ag.run("go");
        chk(final.empty() && llm.calls == 0 && ctx.ended == 1 && ctx.ended_interrupted == 1, "interrupted at the loop top: end_turn once, interrupted, no call");
    }
    {   // begin_turn that throws: the turn never began, nothing to close
        struct BadBegin : Spy {
            void begin_turn(const std::string&) override { throw std::runtime_error("no turn"); }
        } ctx;
        Script llm;
        agent::Agent ag;
        ag.llm = &llm;
        ag.context = &ctx;
        bool thrown = false;
        try {
            ag.run("go");
        } catch (const std::exception&) {
            thrown = true;
        }
        chk(thrown && ctx.ended == 0 && llm.calls == 0, "begin_turn throws: no end_turn, no call");
    }
}

// (e) the budget is a lock-free counter: concurrent consumers never overshoot, refunds give back
static void test_budget() {
    agent::IterationBudget b(2500);
    std::atomic<int> granted{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t)
        ts.emplace_back([&] {
            for (int i = 0; i < 1000; ++i)
                if (b.consume()) ++granted;
        });
    for (auto& t : ts) t.join();
    chk(granted == 2500 && b.used == 2500 && b.remaining() == 0, "budget: 4 threads x 1000 consumes against 2500 grant exactly 2500");
    chk(!b.consume(), "budget: spent");
    b.refund();
    chk(b.remaining() == 1 && b.consume() && !b.consume(), "budget: a refund gives one back");
    agent::IterationBudget z(3);
    z.refund();
    chk(z.used == 0 && z.remaining() == 3, "budget: refund never goes below zero");
    chk(std::atomic<int>::is_always_lock_free, "budget: the counter is lock-free");
}

// (f) a summary answer that is only a tool call is reported as that, not as a transport-less silence
static void test_max_iterations_tool_summary() {
    tools::ToolRegistry reg;
    add_tool(reg, "echo", [](const Json&) { return std::string("{}"); });
    Script llm;
    llm.always_tools = true;
    agent::Agent ag;
    ag.registry = &reg;
    ag.llm = &llm;
    ag.max_iterations = 2;
    const std::string final = ag.run("go");
    chk(final.find("tried to call tools") != std::string::npos && final.find("echo") != std::string::npos,
        "max iterations: a tool-call-only summary is reported, got '" + final + "'");
    chk(llm.calls == 3, "max iterations: two loop calls and one summary call");
}

// (g) the ephemeral prompt (the plan) reaches every call exactly once, behind the tape's
// payload tail with a context engine and behind the standing system prompt without one.
static int count_ephemeral(const std::vector<core::Message>& m, const std::string& text) {
    int n = 0;
    for (const auto& x : m)
        if (x.role == "system" && x.content == text) ++n;
    return n;
}

static void test_ephemeral_prompt_placement() {
    // no context engine: the standing system prompt keeps slot 0, the plan sits behind it
    {
        Script script;
        script.always_tools = false;
        agent::Agent agent;
        agent.system_prompt = "base prompt";
        agent.ephemeral_system_prompt = "ACTIVE PLAN";
        agent.max_iterations = 2;
        tools::ToolRegistry reg;
        add_tool(reg, "echo", [](const Json&) { return std::string("ok"); });
        agent.registry = &reg;
        agent.llm = &script;
        agent.run("go");
        chk(script.requests.size() == 1, "one call for a plain answer");
        const auto& r = script.requests[0];
        chk(r.size() == 3 && r[0].role == "system" && r[0].content == "base prompt", "standing prompt keeps slot 0");
        chk(r[1].role == "system" && r[1].content == "ACTIVE PLAN", "plan sits behind the standing prompt");
        chk(r[2].role == "user", "the user message follows");
        chk(count_ephemeral(r, "ACTIVE PLAN") == 1, "the plan appears exactly once");
    }
    // empty ephemeral: no extra system message at all
    {
        Script script;
        agent::Agent agent;
        agent.system_prompt = "base prompt";
        agent.max_iterations = 2;
        agent.llm = &script;
        agent.run("go");
        chk(script.requests[0].size() == 2, "no ephemeral, no extra message");
    }
    // context engine (a tape): payload wins, plan lands at the tail, never in the payload
    {
        Script script;
        script.always_tools = true;   // one loop call plus the summary call
        Spy spy;
        agent::Agent agent;
        agent.system_prompt = "base prompt";
        agent.ephemeral_system_prompt = "ACTIVE PLAN";
        agent.max_iterations = 1;
        tools::ToolRegistry reg;
        add_tool(reg, "echo", [](const Json&) { return std::string("ok"); });
        agent.registry = &reg;
        agent.llm = &script;
        agent.context = &spy;
        agent.run("go");
        chk(script.calls == 2, "one loop call and the summary call");
        for (const auto& r : script.requests)
            chk(count_ephemeral(r, "ACTIVE PLAN") == 1, "the plan rides every call exactly once");
        const auto& loop = script.requests[0];
        chk(loop.size() == 2 && loop[0].role == "user" && loop[0].content == "go",
            "payload wins over messages");
        chk(loop[1].role == "system" && loop[1].content == "ACTIVE PLAN",
            "under a tape the plan rides the payload tail");
        const auto& summary = script.requests.back();
        chk(summary.size() == 3 && summary[0].role == "user" &&
            summary[1].role == "system" && summary[1].content == "ACTIVE PLAN" &&
            summary[2].role == "user" && summary[2].content.find("maximum") != std::string::npos,
            "the summary call rides the tape, the plan, then its own request");
    }
}

int main() {
    test_interrupt_sequential();
    test_interrupt_parallel();
    test_interrupt_before_batch();
    test_tool_delay();
    test_dedup_before_recording();
    test_end_turn();
    test_budget();
    test_max_iterations_tool_summary();
    test_ephemeral_prompt_placement();
    std::cout << g_pass << " passed, " << g_fail << " failed\n";
    if (g_fail == 0) std::cout << "all agent run tests passed\n";
    return g_fail ? 1 : 0;
}
