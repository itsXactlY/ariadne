// tests/agent_loop_test.cpp — agent run loop + helpers against a scripted LLM.
#include <atomic>
#include <iostream>
#include <optional>
#include <vector>
#include <memory>
#include <stdexcept>
#include <string>
#include "dd/agent/agent.hpp"
#include "dd/agent/iteration_budget.hpp"
#include "dd/core/message.hpp"
#include "dd/core/json.hpp"
#include "dd/tools/registry.hpp"
#include "dd/tools/tool.hpp"

using namespace dd;
using dd::core::Json;

#define CHECK(c)                                                             \
    do {                                                                     \
        if (!(c)) {                                                          \
            std::cerr << "FAIL: " << __func__ << " line " << __LINE__ << "\n"; \
            return 1;                                                        \
        }                                                                    \
    } while (0)

struct Mock : agent::LLMClient {
    int calls = 0;
    core::Message chat(const std::vector<core::Message>& msgs, const core::Json& tools) override {
        (void)msgs;
        ++calls;
        if (tools.type == Json::Arr && tools.arr.empty()) {
            core::Message m;
            m.role = "assistant";
            m.content = "final summary";
            m.finish_reason = "stop";
            return m;
        }
        if (calls == 1) {
            core::Message m;
            m.role = "assistant";
            m.tool_calls = {
                {"c1", "echo", "{\"x\":1}"},
                {"c2", "echo", "{\"x\":1}"},
                {"c3", "read_file", "{\"path\":\"/a/b\"}"},
            };
            return m;
        }
        core::Message m;
        m.role = "assistant";
        m.content = "done";
        m.finish_reason = "stop";
        return m;
    }
};

static int test_helpers() {
    std::vector<core::ToolCall> calls = {
        {"c1", "echo", "{\"x\":1}"},
        {"c2", "echo", "{\"x\":1}"},
        {"c3", "read_file", "{\"path\":\"/a\"}"},
    };
    auto d = agent::dedup_tool_calls(calls);
    CHECK(d.size() == 2);
    CHECK(d[0].id == "c1" && d[1].id == "c3");

    std::vector<core::ToolCall> dels;
    for (int i = 0; i < 5; ++i) dels.push_back({"d" + std::to_string(i), "delegate_task", "{}"});
    dels.push_back({"e", "echo", "{}"});
    auto capped = agent::cap_delegate_calls(dels, 3);
    CHECK(capped.size() == 4);
    int kept = 0;
    for (auto& t : capped) if (t.name == "delegate_task") ++kept;
    CHECK(kept == 3);

    CHECK(agent::get_budget_warning(9, 10, 0.9, 0.75).find("BUDGET WARNING") != std::string::npos);
    CHECK(agent::get_budget_warning(8, 10, 0.9, 0.75).find("BUDGET:") != std::string::npos);
    CHECK(agent::get_budget_warning(8, 10, 0.9, 0.75).find("WARNING") == std::string::npos);
    CHECK(agent::get_budget_warning(2, 10, 0.9, 0.75).empty());

    std::vector<core::ToolCall> p1 = {
        {"a", "read_file", "{\"path\":\"/x/f1\"}"},
        {"b", "read_file", "{\"path\":\"/y/f2\"}"},
    };
    CHECK(agent::should_parallelize(p1, nullptr));
    std::vector<core::ToolCall> p2 = {
        {"a", "read_file", "{\"path\":\"/x\"}"},
        {"b", "read_file", "{\"path\":\"/x/f1\"}"},
    };
    CHECK(!agent::should_parallelize(p2, nullptr));
    std::vector<core::ToolCall> p3 = {
        {"a", "clarify", "{}"},
        {"b", "web_search", "{\"q\":\"x\"}"},
    };
    CHECK(!agent::should_parallelize(p3, nullptr));
    std::vector<core::ToolCall> p4 = {{"a", "read_file", "{\"path\":\"/x\"}"}};
    CHECK(!agent::should_parallelize(p4, nullptr));
    std::vector<core::ToolCall> p5 = {{"a", "weird", "{}"}, {"b", "weird2", "{}"}};
    CHECK(!agent::should_parallelize(p5, nullptr));
    return 0;
}

static int test_run_loop() {
    tools::ToolRegistry reg;
    tools::ToolEntry echo;
    echo.name = "echo";
    echo.handler = [](const Json& args) { return std::string("echoed:") + core::json_dump(args); };
    reg.register_tool(echo);
    tools::ToolEntry rf;
    rf.name = "read_file";
    rf.handler = [](const Json&) { return std::string("file-contents"); };
    reg.register_tool(rf);

    Mock mock;
    agent::Agent a;
    a.registry = &reg;
    a.llm = &mock;
    a.max_iterations = 10;
    a.tool_delay = 0.0;
    a.system_prompt = "sys";

    std::string final = a.run("hello");
    CHECK(final == "done");
    CHECK(a.api_call_count == 2);
    CHECK(a.budget->used == 2);
    CHECK(mock.calls == 2);
    return 0;
}

static int test_max_iterations() {
    tools::ToolRegistry reg;
    tools::ToolEntry echo;
    echo.name = "echo";
    echo.handler = [](const Json&) { return std::string("r"); };
    reg.register_tool(echo);

    struct AlwaysTool : agent::LLMClient {
        int n = 0;
        core::Message chat(const std::vector<core::Message>&, const core::Json& tools) override {
            ++n;
            if (tools.type == Json::Arr && tools.arr.empty()) {
                core::Message m;
                m.role = "assistant";
                m.content = "final summary";
                m.finish_reason = "stop";
                return m;
            }
            core::Message m;
            m.role = "assistant";
            m.tool_calls = {{"t1", "echo", "{}"}};
            return m;
        }
    } always;

    agent::Agent a;
    a.registry = &reg;
    a.llm = &always;
    a.max_iterations = 2;
    a.tool_delay = 0.0;

    std::string final = a.run("go");
    CHECK(final == "final summary");
    CHECK(a.api_call_count == 2);
    CHECK(always.n == 3);
    return 0;
}

// A request the server refused for the window goes out once more, rebuilt by the context engine: not repeated
// unchanged (the same prompt fails the same way), not retried forever.
struct Refusing : agent::LLMClient {
    int calls = 0, refuse = 1;   // refuse: how many calls fail before one answers
    core::Message chat(const std::vector<core::Message>&, const core::Json&) override {
        if (++calls <= refuse)
            throw std::runtime_error("HTTP 400: request exceeds the context window: prompt (61447 tokens) leaves no "
                                     "room to answer in the context (61337); requests are never truncated");
        core::Message m;
        m.role = "assistant";
        m.content = "done";
        m.finish_reason = "stop";
        return m;
    }
};

struct Rebuilder : agent::ContextEngine {
    bool fixes = true;
    int overflows = 0, payloads = 0;
    void begin_turn(const std::string&) override {}
    std::vector<core::Message> payload() override {
        ++payloads;
        core::Message u;
        u.role = "user";
        u.content = "go";
        return {u};
    }
    core::Json tools() override { return core::json_arr(); }
    void on_assistant(const core::Message&) override {}
    void on_tool_results(const std::vector<core::Message>&) override {}
    void after_tools() override {}
    void end_turn(const std::string&, bool) override {}
    bool on_overflow(const std::string&) override {
        ++overflows;
        return fixes;
    }
};

static int test_overflow_retry() {
    {   // the engine rebuilt the history: one more call, which answers
        Refusing llm;
        Rebuilder ctx;
        agent::Agent a;
        a.llm = &llm;
        a.context = &ctx;
        CHECK(a.run("go") == "done");
        CHECK(llm.calls == 2 && ctx.overflows == 1 && ctx.payloads == 2 && a.api_call_count == 1);
    }
    {   // the engine could not make room: the error stands, nothing is sent again
        Refusing llm;
        Rebuilder ctx;
        ctx.fixes = false;
        agent::Agent a;
        a.llm = &llm;
        a.context = &ctx;
        bool thrown = false;
        try {
            a.run("go");
        } catch (const std::runtime_error& e) {
            thrown = std::string(e.what()).find("exceeds the context window") != std::string::npos;
        }
        CHECK(thrown && llm.calls == 1 && ctx.overflows == 1);
    }
    {   // refused again after the rebuild: one retry, then the error (never a loop)
        Refusing llm;
        llm.refuse = 5;
        Rebuilder ctx;
        agent::Agent a;
        a.llm = &llm;
        a.context = &ctx;
        bool thrown = false;
        try {
            a.run("go");
        } catch (const std::runtime_error&) {
            thrown = true;
        }
        CHECK(thrown && llm.calls == 2 && ctx.overflows == 1);
    }
    {   // without a context engine an error is the caller's, as before
        Refusing llm;
        agent::Agent a;
        a.llm = &llm;
        bool thrown = false;
        try {
            a.run("go");
        } catch (const std::runtime_error&) {
            thrown = true;
        }
        CHECK(thrown && llm.calls == 1);
    }
    return 0;
}

// A gate can refuse a call before it runs: the model gets the gate's text as that call's result, the
// handler never runs, the rest of the batch does. Sequential and parallel batches alike.
struct OneRefusal : agent::ToolGate {
    std::string name;   // refuses the first call of this tool
    int seen = 0;
    bool refused = false;
    std::optional<std::string> veto(const core::ToolCall& c) override {
        ++seen;
        if (c.name == name && !refused) {
            refused = true;
            return std::string("{\"error\":\"[rule held] not like this\"}");
        }
        return std::nullopt;
    }
};

struct TwoCalls : agent::LLMClient {
    std::string a, b;
    std::string args_a, args_b;
    int round = 0;
    core::Message chat(const std::vector<core::Message>&, const core::Json&) override {
        core::Message m;
        m.role = "assistant";
        if (round++ == 0) {
            m.tool_calls = {{"c1", a, args_a}, {"c2", b, args_b}};
            return m;
        }
        m.content = "done";
        m.finish_reason = "stop";
        return m;
    }
};

static int run_gate_case(const std::string& a, const std::string& args_a, const std::string& b, const std::string& args_b, const char* label) {
    tools::ToolRegistry reg;
    std::atomic<int> ran_a{0}, ran_b{0};
    for (const auto& [name, counter] : {std::pair<std::string, std::atomic<int>*>{a, &ran_a}, std::pair<std::string, std::atomic<int>*>{b, &ran_b}}) {
        if (name == b && a == b) continue;
        tools::ToolEntry e;
        e.name = name;
        e.handler = [counter](const Json&) {
            ++*counter;
            return std::string("{\"ok\":true}");
        };
        reg.register_tool(e);
    }
    TwoCalls llm;
    llm.a = a;
    llm.b = b;
    llm.args_a = args_a;
    llm.args_b = args_b;
    OneRefusal gate;
    gate.name = a;
    agent::Agent ag;
    ag.registry = &reg;
    ag.llm = &llm;
    ag.gate = &gate;
    ag.tool_delay = 0.0;
    std::vector<std::string> results;
    ag.step_hook = [&](const core::Message& m) {
        if (m.role == "tool") results.push_back(m.content);
    };
    const std::string final = ag.run("go");
    if (final != "done" || gate.seen != 2 || ran_a != 0 || ran_b != 1 || results.size() != 2 ||
        results[0].find("[rule held]") == std::string::npos || results[1].find("ok") == std::string::npos) {
        std::cerr << "FAIL: gate " << label << ": final=" << final << " seen=" << gate.seen << " ran_a=" << ran_a << " ran_b=" << ran_b << " results=" << results.size() << "\n";
        return 1;
    }
    return 0;
}

static int test_gate() {
    if (run_gate_case("echo", "{\"x\":1}", "weird", "{}", "sequential batch")) return 1;
    if (run_gate_case("read_file", "{\"path\":\"/x/f1\"}", "list_dir", "{\"path\":\"/y\"}", "parallel batch")) return 1;
    {   // no gate: as before
        tools::ToolRegistry reg;
        std::atomic<int> ran{0};
        tools::ToolEntry e;
        e.name = "echo";
        e.handler = [&](const Json&) {
            ++ran;
            return std::string("{}");
        };
        reg.register_tool(e);
        TwoCalls llm;
        llm.a = "echo";
        llm.b = "echo";
        llm.args_a = "{\"x\":1}";
        llm.args_b = "{\"x\":2}";
        agent::Agent ag;
        ag.registry = &reg;
        ag.llm = &llm;
        ag.tool_delay = 0.0;
        if (ag.run("go") != "done" || ran != 2) {
            std::cerr << "FAIL: no gate changes nothing\n";
            return 1;
        }
    }
    return 0;
}

int main() {
    if (test_helpers()) return 1;
    if (test_run_loop()) return 1;
    if (test_max_iterations()) return 1;
    if (test_overflow_retry()) return 1;
    if (test_gate()) return 1;
    std::cout << "all agent-loop tests passed\n";
    return 0;
}
