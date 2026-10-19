// Differential test for the pure acp_adapter/tools.py subset (get_tool_kind,
// build_tool_title, extract_locations) vs captured Python ground truth.
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "dd/core/json.hpp"
#include "dd/acp/tools.hpp"

using namespace dd;
using namespace dd::core;
using namespace dd::acp;

static int failures = 0;
static void chk(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::cout << "FAIL " << what << "\n";
    }
}

// build an empty-object args Json and let callers populate it.
static Json mkargs() { return json_obj(); }

int main() {
    // ---- get_tool_kind ----
    chk(get_tool_kind("read_file") == "read", "kind:read_file");
    chk(get_tool_kind("write_file") == "edit", "kind:write_file");
    chk(get_tool_kind("patch") == "edit", "kind:patch");
    chk(get_tool_kind("search_files") == "search", "kind:search_files");
    chk(get_tool_kind("terminal") == "execute", "kind:terminal");
    chk(get_tool_kind("web_search") == "fetch", "kind:web_search");
    chk(get_tool_kind("browser_snapshot") == "read", "kind:browser_snapshot");
    chk(get_tool_kind("_thinking") == "think", "kind:_thinking");
    chk(get_tool_kind("nope") == "other", "kind:unknown");

    // ---- build_tool_title ----
    {
        Json a = mkargs();
        obj_set(a, "command", json_str(std::string(100, 'x')));
        chk(build_tool_title("terminal", a) ==
                "terminal: " + std::string(77, 'x') + "...", "title:terminal-trunc");
        Json a2 = mkargs();
        obj_set(a2, "command", json_str("ls -la"));
        chk(build_tool_title("terminal", a2) == "terminal: ls -la", "title:terminal");
    }
    {
        Json a = mkargs();
        obj_set(a, "path", json_str("/a/b"));
        chk(build_tool_title("read_file", a) == "read: /a/b", "title:read_file");
        chk(build_tool_title("read_file", mkargs()) == "read: ?", "title:read_file-def");
        chk(build_tool_title("read_file", [&] { Json a3 = mkargs(); obj_set(a3, "path", json_num(5)); return a3; }()) ==
                "read: 5", "title:read_file-int");
    }
    {
        Json a = mkargs();
        obj_set(a, "mode", json_str("insert"));
        obj_set(a, "path", json_str("f"));
        chk(build_tool_title("patch", a) == "patch (insert): f", "title:patch-mode");
        Json a2 = mkargs();
        obj_set(a2, "path", json_str("f"));
        chk(build_tool_title("patch", a2) == "patch (replace): f", "title:patch-def");
    }
    {
        Json a = mkargs();
        obj_set(a, "pattern", json_str("needle"));
        chk(build_tool_title("search_files", a) == "search: needle", "title:search_files");
        Json b = mkargs();
        obj_set(b, "query", json_str("q"));
        chk(build_tool_title("web_search", b) == "web search: q", "title:web_search");
    }
    {
        Json a = mkargs();
        Json urls = json_arr();
        arr_push(urls, json_str("u1"));
        arr_push(urls, json_str("u2"));
        arr_push(urls, json_str("u3"));
        obj_set(a, "urls", urls);
        chk(build_tool_title("web_extract", a) == "extract: u1 (+2)", "title:web_extract-3");
        Json b = mkargs();
        Json u2 = json_arr();
        arr_push(u2, json_str("only"));
        obj_set(b, "urls", u2);
        chk(build_tool_title("web_extract", b) == "extract: only", "title:web_extract-1");
        chk(build_tool_title("web_extract", mkargs()) == "web extract", "title:web_extract-def");
    }
    {
        Json a = mkargs();
        obj_set(a, "goal", json_str(std::string(100, 'g')));
        chk(build_tool_title("delegate_task", a) == "delegate: " + std::string(57, 'g') + "...", "title:delegate-trunc");
        Json b = mkargs();
        obj_set(b, "goal", json_str(""));
        chk(build_tool_title("delegate_task", b) == "delegate task", "title:delegate-empty");
        chk(build_tool_title("delegate_task", mkargs()) == "delegate task", "title:delegate-def");
    }
    chk(build_tool_title("execute_code", mkargs()) == "execute code", "title:execute_code");
    {
        Json a = mkargs();
        obj_set(a, "question", json_str(std::string(80, 'q')));
        chk(build_tool_title("vision_analyze", a) == "analyze image: " + std::string(50, 'q'), "title:vision-trunc");
    }
    chk(build_tool_title("unknown_tool", mkargs()) == "unknown_tool", "title:passthrough");

    // ---- extract_locations ----
    auto loc_of = [](const std::string& path, const std::optional<std::string>& line) {
        return ToolCallLocation{path, line};
    };
    {
        Json a = mkargs();
        obj_set(a, "path", json_str("a.py"));
        obj_set(a, "offset", json_num(10));
        auto v = extract_locations(a);
        chk(v.size() == 1 && v[0].path == "a.py" && v[0].line == std::optional<std::string>("10"), "loc:offset10");
    }
    {
        Json a = mkargs();
        obj_set(a, "path", json_str("b.py"));
        obj_set(a, "line", json_num(7));
        auto v = extract_locations(a);
        chk(v.size() == 1 && v[0].path == "b.py" && v[0].line == std::optional<std::string>("7"), "loc:line7");
    }
    {
        // offset falsy (0) -> falls back to line=9
        Json a = mkargs();
        obj_set(a, "path", json_str("c.py"));
        obj_set(a, "offset", json_num(0));
        obj_set(a, "line", json_num(9));
        auto v = extract_locations(a);
        chk(v.size() == 1 && v[0].path == "c.py" && v[0].line == std::optional<std::string>("9"), "loc:offset0->line9");
    }
    chk(extract_locations([&] { Json a = mkargs(); obj_set(a, "path", json_str("")); return a; }()).size() == 0, "loc:empty-path");
    chk(extract_locations(mkargs()).size() == 0, "loc:no-keys");
    {
        Json a = mkargs();
        obj_set(a, "path", json_str("d.py"));
        auto v = extract_locations(a);
        chk(v.size() == 1 && v[0].path == "d.py" && !v[0].line.has_value(), "loc:d-no-line");
    }
    {
        // offset wins over line when truthy
        Json a = mkargs();
        obj_set(a, "path", json_str("e.py"));
        obj_set(a, "offset", json_num(3));
        obj_set(a, "line", json_num(4));
        auto v = extract_locations(a);
        chk(v.size() == 1 && v[0].path == "e.py" && v[0].line == std::optional<std::string>("3"), "loc:offset-wins");
    }
    {
        // line=0 is present-but-falsy -> still recorded (0)
        Json a = mkargs();
        obj_set(a, "path", json_str("f.py"));
        obj_set(a, "line", json_num(0));
        auto v = extract_locations(a);
        chk(v.size() == 1 && v[0].path == "f.py" && v[0].line == std::optional<std::string>("0"), "loc:line0");
    }
    {
        Json a = mkargs();
        obj_set(a, "path", json_str("g.py"));
        obj_set(a, "offset", json_str("12"));
        auto v = extract_locations(a);
        chk(v.size() == 1 && v[0].path == "g.py" && v[0].line == std::optional<std::string>("12"), "loc:offset-str");
    }

    (void)loc_of;  // helper kept for readability
    if (failures == 0) {
        std::cout << "all acp tests passed\n";
        return 0;
    }
    std::cout << failures << " acp tests FAILED\n";
    return 1;
}
