// Ground-truth cross-checks for the todo_tool port (verified against
// Python tools/todo_tool.py in-session).
#include <iostream>
#include <string>
#include <vector>

#include "dd/core/json.hpp"
#include "dd/tools/todo_tool.hpp"

using namespace dd;
using namespace dd::tools;
using namespace dd::core;

static int g_fail = 0;

#define CHECK(cond, label)                                          \
    do {                                                            \
        if (!(cond)) {                                              \
            ++g_fail;                                               \
            std::cout << "FAIL " << (label) << "\n";                \
        }                                                           \
    } while (0)

static const std::string IN1 =
    R"JSON([{"id":"1","content":"do X","status":"pending"},
           {"id":"2","content":"  spaced  ","status":"DONE"},
           {"id":"","content":"","status":"weird"}])JSON";

static const std::string EXP1 =
    "{\"todos\": [{\"id\": \"1\", \"content\": \"do X\", \"status\": \"pending\"}, "
    "{\"id\": \"2\", \"content\": \"spaced\", \"status\": \"pending\"}, "
    "{\"id\": \"?\", \"content\": \"(no description)\", \"status\": \"pending\"}], "
    "\"summary\": {\"total\": 3, \"pending\": 3, \"in_progress\": 0, \"completed\": 0, \"cancelled\": 0}}";

static const std::string FMT1 =
    "[Your active task list was preserved across context compression]\n"
    "- [ ] 1. do X (pending)\n"
    "- [ ] 2. spaced (pending)\n"
    "- [ ] ?. (no description) (pending)";

static const std::string IN2 =
    R"JSON([{"id":"1","content":"new X","status":"completed"},
           {"id":"3","content":"three"},
           {"content":"no id"}])JSON";

static const std::string EXP2 =
    "{\"todos\": [{\"id\": \"1\", \"content\": \"new X\", \"status\": \"completed\"}, "
    "{\"id\": \"2\", \"content\": \"spaced\", \"status\": \"pending\"}, "
    "{\"id\": \"?\", \"content\": \"(no description)\", \"status\": \"pending\"}, "
    "{\"id\": \"3\", \"content\": \"three\", \"status\": \"pending\"}], "
    "\"summary\": {\"total\": 4, \"pending\": 3, \"in_progress\": 0, \"completed\": 1, \"cancelled\": 0}}";

static const std::string FMT2 =
    "[Your active task list was preserved across context compression]\n"
    "- [ ] 2. spaced (pending)\n"
    "- [ ] ?. (no description) (pending)\n"
    "- [ ] 3. three (pending)";

static const std::string IN4 =
    R"JSON([{"id":"a","content":"alpha","status":"in_progress"},
           {"id":"b","content":"beta","status":"completed"},
           {"id":"c","content":"gamma","status":"CANCELLED"},
           {"id":"d","content":"delta","status":"pending"}])JSON";

static const std::string EXP4 =
    "{\"todos\": [{\"id\": \"a\", \"content\": \"alpha\", \"status\": \"in_progress\"}, "
    "{\"id\": \"b\", \"content\": \"beta\", \"status\": \"completed\"}, "
    "{\"id\": \"c\", \"content\": \"gamma\", \"status\": \"cancelled\"}, "
    "{\"id\": \"d\", \"content\": \"delta\", \"status\": \"pending\"}], "
    "\"summary\": {\"total\": 4, \"pending\": 1, \"in_progress\": 1, \"completed\": 1, \"cancelled\": 1}}";

static const std::string FMT4 =
    "[Your active task list was preserved across context compression]\n"
    "- [>] a. alpha (in_progress)\n"
    "- [ ] d. delta (pending)";

static const std::string IN5 =
    R"JSON([{"id":"1","content":"one","status":"pending"},{"id":"2","content":"two"}])JSON";

static const std::string EXP5 =
    "{\"todos\": [{\"id\": \"1\", \"content\": \"one\", \"status\": \"pending\"}, "
    "{\"id\": \"2\", \"content\": \"two\", \"status\": \"pending\"}], "
    "\"summary\": {\"total\": 2, \"pending\": 2, \"in_progress\": 0, \"completed\": 0, \"cancelled\": 0}}";

static const std::string IN6 =
    R"JSON([{"id":"1","status":"bogus"},{"id":"2","content":""},{"id":"2","content":"two2"}])JSON";

static const std::string EXP6 =
    "{\"todos\": [{\"id\": \"1\", \"content\": \"one\", \"status\": \"pending\"}, "
    "{\"id\": \"2\", \"content\": \"two2\", \"status\": \"pending\"}], "
    "\"summary\": {\"total\": 2, \"pending\": 2, \"in_progress\": 0, \"completed\": 0, \"cancelled\": 0}}";

int main() {
    // Case 1: replace-write with normalization + invalid statuses.
    {
        TodoStore s;
        core::Json todos = json_parse(IN1);
        std::string got = todo_tool(todos, false, s);
        CHECK(got == EXP1, "todo1.replace");
        auto fmt = s.format_for_injection();
        CHECK(fmt.has_value() && *fmt == FMT1, "todo1.format");
    }
    // Case 2: merge-update existing id, append new id, skip empty id.
    {
        TodoStore s;
        s.write(json_parse(IN1), false);
        std::string got = todo_tool(json_parse(IN2), true, s);
        CHECK(got == EXP2, "todo2.merge");
        auto fmt = s.format_for_injection();
        CHECK(fmt.has_value() && *fmt == FMT2, "todo2.format");
        std::string readback = todo_tool(core::Json{}, false, s);
        CHECK(readback == EXP2, "todo2.read-eq-write");
    }
    // Case 4: status markers + active-only filtering.
    {
        TodoStore s;
        std::string got = todo_tool(json_parse(IN4), false, s);
        CHECK(got == EXP4, "todo4.statuses");
        auto fmt = s.format_for_injection();
        CHECK(fmt.has_value() && *fmt == FMT4, "todo4.format");
    }
    // Empty store: read + no format.
    {
        TodoStore s;
        std::string got = todo_tool(core::Json{}, false, s);
        CHECK(got == "{\"todos\": [], \"summary\": {\"total\": 0, \"pending\": 0, \"in_progress\": 0, \"completed\": 0, \"cancelled\": 0}}",
              "todo.empty-read");
        CHECK(!s.has_items(), "todo.empty-has-items");
        CHECK(!s.format_for_injection().has_value(), "todo.empty-format-none");
    }
    // Merge semantics: invalid status ignored, empty content skipped, later wins.
    {
        TodoStore s;
        CHECK(todo_tool(json_parse(IN5), false, s) == EXP5, "todo5.replace");
        CHECK(todo_tool(json_parse(IN6), true, s) == EXP6, "todo6.merge-ignore");
    }

    if (g_fail == 0) {
        std::cout << "all todo-tool tests passed\n";
        return 0;
    }
    std::cout << g_fail << " todo-tool test(s) FAILED\n";
    return 1;
}
