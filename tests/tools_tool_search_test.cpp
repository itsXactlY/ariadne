// Ground-truth cross-checks for the pure tool_search lexical/config port
// (verified against Python tools/tool_search.py in-session).
#include <iostream>
#include <string>
#include <vector>
#include <optional>

#include "dd/core/json.hpp"
#include "dd/tools/tool_search.hpp"

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

static const std::string BR2 = [] {
    const std::string BR2RAW =
    R"BR([{"type":"function","function":{"name":"tool_search","description":"Search the 2 tools that are not loaded in this context. Returns matching names with their descriptions; follow with `tool_describe` for a tool's parameter schema, then `tool_call` to invoke it. All 2 are connected and callable — never report a capability as missing until a search for it returned no match. Tools already listed in this context are loaded and must be called directly, not searched.","parameters":{"type":"object","properties":{"query":{"type":"string","description":"Keywords describing the capability you need (e.g. 'create github issue')."},"limit":{"type":"integer","description":"Maximum number of results to return. Default 5."}},"required":["query"]}}},{"type":"function","function":{"name":"tool_describe","description":"Load the full JSON schema for one tool returned by `tool_search`. Required before `tool_call` if the tool's parameters are unknown.","parameters":{"type":"object","properties":{"name":{"type":"string","description":"Exact tool name (as returned by tool_search)."}},"required":["name"]}}},{"type":"function","function":{"name":"tool_call","description":"Invoke a deferred tool by name with the given arguments. Argument shape matches the tool's schema (see `tool_describe`). Policy, hooks, and approvals run exactly as for any directly-listed tool.","parameters":{"type":"object","properties":{"name":{"type":"string","description":"Exact tool name to invoke."},"arguments":{"type":"object","description":"Arguments for the tool, matching its schema."}},"required":["name","arguments"]}}}]
)BR";
    std::string s = BR2RAW;
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}();

static std::vector<CatalogEntry> make_cat() {
    std::vector<CatalogEntry> cat;
    cat.push_back(CatalogEntry{"create_github_issue", "open a github issue", "plugin", "github",
                   {"create", "github", "issue", "open", "a", "github", "issue"}});
    cat.push_back(CatalogEntry{"list_github_prs", "list pull requests on github", "plugin", "github",
                   {"list", "github", "prs", "list", "pull", "requests", "on", "github"}});
    cat.push_back(CatalogEntry{"slack_post", "post a slack message", "plugin", "slack",
                   {"slack", "post", "post", "a", "slack", "message"}});
    return cat;
}

int main() {
    // tokenize
    {
        auto t = tokenize("Hello_world Foo-Bar.baz 123");
        std::vector<std::string> exp = {"hello", "world", "foo", "bar", "baz", "123"};
        CHECK(t == exp, "tokenize.basic");
        CHECK(tokenize("").empty(), "tokenize.empty");
    }

    // entry_search_text + token estimate
    {
        core::Json td = json_parse(R"JSON({"type":"function","function":{"name":"create_github-issue","description":"Make an ISSUE","parameters":{"type":"object","properties":{"title":{"type":"string"},"body":{"type":"string"}},"required":["title"]}}})JSON");
        CHECK(entry_search_text(td) == "create github issue Make an ISSUE title body", "entry_search_text");
        CHECK(estimate_tokens_from_schemas({td}) == 53, "estimate_tokens");
    }

    // is_bridge_tool
    {
        CHECK(is_bridge_tool("tool_search") && is_bridge_tool("tool_describe") && is_bridge_tool("tool_call"),
              "bridge.yes");
        CHECK(!is_bridge_tool("read_file"), "bridge.no");
    }

    // name_affinity
    {
        CHECK(name_affinity("create github issue", tokenize("create github issue"), "create_github-issue") == 1.0, "aff.exact-subset");
        CHECK(name_affinity("github", tokenize("github"), "create_github-issue") == 2.0, "aff.substr");
        CHECK(name_affinity("x", tokenize("x"), "create_github-issue") == 0.0, "aff.none");
        CHECK(name_affinity("Foo_Bar", tokenize("x"), "foo-bar") == 0.0, "aff.mixed");
    }

    // source_label
    {
        CHECK(source_label("mcp-github") == "github", "src.mcp");
        CHECK(source_label("github") == "github", "src.plain");
        CHECK(source_label("") == "other", "src.empty");
    }

    // search_catalog + format_search_hit + available_source_summary
    {
        auto cat = make_cat();
        auto hits = search_catalog(cat, "github issue", 5);
        CHECK(hits.size() == 2 && hits[0].name == "create_github_issue" && hits[1].name == "list_github_prs",
              "search.github-issue");
        auto h2 = search_catalog(cat, "slack", 1);
        CHECK(h2.size() == 1 && h2[0].name == "slack_post", "search.slack");
        CHECK(search_catalog(cat, "zzz", 5).empty(), "search.nomatch");
        const std::string fs0 =
            R"({"name":"create_github_issue","source":"plugin","source_name":"github","description":"open a github issue"})";
        CHECK(json_dump(format_search_hit(cat[0])) == fs0, "format_hit");
        const std::string as0 = R"([{"name":"github","tool_count":2},{"name":"slack","tool_count":1}])";
        CHECK(json_dump(available_source_summary(cat)) == as0, "source_summary");
    }

    // rank_documents
    {
        std::vector<std::pair<std::string, std::string>> docs = {
            {"create-issue", "create a new issue on github"},
            {"list-prs", "list pull requests"}};
        auto r = rank_documents(docs, "github issue", 20);
        CHECK(!r.empty() && r[0].first == "create-issue", "rank.top");
    }

    // config from_raw
    {
        auto c = ToolSearchConfig::from_raw(json_bool(true));
        CHECK(c.enabled == "auto" && c.threshold_pct == 5.0 && c.search_default_limit == 5 &&
              c.max_search_limit == 20 && c.always_load.empty(), "cfg.true");
        c = ToolSearchConfig::from_raw(json_bool(false));
        CHECK(c.enabled == "off", "cfg.false");
        c = ToolSearchConfig::from_raw(json_null());
        CHECK(c.enabled == "auto" && c.threshold_pct == 5.0 && c.max_search_limit == 20, "cfg.none");

        core::Json raw = json_parse(R"JSON({"enabled":"TRUE","threshold_pct":"150","max_search_limit":"99","search_default_limit":"0","always_load":["a ",""," b",5]})JSON");
        c = ToolSearchConfig::from_raw(raw);
        const std::vector<std::string> want = {"a", "b"};
        CHECK(c.enabled == "on" && c.threshold_pct == 100.0 && c.max_search_limit == 50 &&
              c.search_default_limit == 1 && c.always_load == want, "cfg.coerce");

        core::Json raw2 = json_parse(R"JSON({"enabled":"weird","always_load":"solo"})JSON");
        c = ToolSearchConfig::from_raw(raw2);
        const std::vector<std::string> want2 = {"solo"};
        CHECK(c.enabled == "auto" && c.threshold_pct == 5.0 && c.max_search_limit == 20 &&
              c.always_load == want2, "cfg.weird+str-always");
    }

    // should_activate
    {
        auto on = ToolSearchConfig::from_raw(json_bool(true));
        auto off = ToolSearchConfig::from_raw(json_bool(false));
        CHECK(should_activate(on, 100, 8000), "activate.on");
        CHECK(!should_activate(off, 100, 8000), "activate.off");
        CHECK(!should_activate(on, 0, 8000), "activate.zero");
        CHECK(should_activate(on, 50, std::nullopt), "activate.noctx");
    }

    // bridge_tool_schemas
    {
        CHECK(json_dump(bridge_tool_schemas(2)) == BR2, "bridge.schemas");
    }

    if (g_fail == 0) {
        std::cout << "all tool_search tests passed\n";
        return 0;
    }
    std::cout << g_fail << " tool_search test(s) FAILED\n";
    return 1;
}