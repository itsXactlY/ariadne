// tests/agent_loop_test.cpp — agent run loop + helpers against a scripted LLM.
#include <iostream>
#include <vector>
#include <memory>
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

int main() {
    if (test_helpers()) return 1;
    if (test_run_loop()) return 1;
    if (test_max_iterations()) return 1;
    std::cout << "all agent-loop tests passed\n";
    return 0;
}
