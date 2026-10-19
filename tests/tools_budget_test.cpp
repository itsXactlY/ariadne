// tests/tools_budget_test.cpp -> budget_config.py
#include "dd/tools/budget_config.hpp"
#include "dd/tools/registry.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace dd::tools;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL: %s\n", #cond); } } while (0)

int main() {
    ToolRegistry reg;

    // "search" registered with an explicit per-tool max_result_size.
    {
        ToolEntry e;
        e.name = "search";
        e.max_result_size_chars = 5000;
        reg.register_tool(e);
    }
    // "plain" registered WITHOUT a per-tool size -> falls through to default.
    {
        ToolEntry e;
        e.name = "plain";
        reg.register_tool(e);
    }
    // "read_file" registered but pinned wins regardless.
    {
        ToolEntry e;
        e.name = "read_file";
        e.max_result_size_chars = 999;
        reg.register_tool(e);
    }

    BudgetConfig b;
    b.tool_overrides["grep"] = 2500;

    // pinned -> infinity
    double rf = b.resolve_threshold("read_file", reg);
    CHECK(std::isinf(rf));

    // override -> 2500
    CHECK(b.resolve_threshold("grep", reg) == 2500.0);

    // registry per-tool -> 5000
    CHECK(b.resolve_threshold("search", reg) == 5000.0);

    // registered but no per-tool size -> default_result_size
    CHECK(b.resolve_threshold("plain", reg) == (double)b.default_result_size);

    // unregistered -> default_result_size
    CHECK(b.resolve_threshold("unknown", reg) == (double)b.default_result_size);

    // DEFAULT_BUDGET has no overrides: grep falls to registry/default, not 2500.
    CHECK(DEFAULT_BUDGET.resolve_threshold("grep", reg) == (double)DEFAULT_BUDGET.default_result_size);
    CHECK(DEFAULT_BUDGET.resolve_threshold("search", reg) == 5000.0);
    CHECK(std::isinf(DEFAULT_BUDGET.resolve_threshold("read_file", reg)));

    // defaults
    CHECK(DEFAULT_BUDGET.default_result_size == 100000);
    CHECK(DEFAULT_BUDGET.turn_budget == 200000);
    CHECK(DEFAULT_BUDGET.preview_size == 1500);

    if (failures == 0) {
        std::printf("all budget-config tests passed\n");
        return 0;
    }
    std::printf("%d failures\n", failures);
    return 1;
}
