// Tests for the pure mcp_schema_cache helpers (config_fingerprint + accessors).
#include "dd/tools/mcp_schema_cache.hpp"
#include "dd/core/json.hpp"
#include <cstdio>
#include <string>
#include <vector>

using namespace dd;
using core::Json;
using core::JsonSP;

static int g_fail = 0;
static void chk(bool ok, const char* label) {
    if (!ok) { std::printf("FAIL %s\n", label); ++g_fail; }
}

int main() {
    // config_fingerprint: parse config JSON, compare 16-char hex to Python ground truth.
    struct Case { const char* cfg; const char* want; };
    const Case cases[] = {
        {"{}", "38953fe0cbe0ee5f"},
        {"{\"command\":\"npx\",\"args\":[\"-y\",\"foo\"],\"url\":null,\"transport\":null,\"tools\":{\"include\":[\"b\",\"a\"],\"exclude\":[\"c\"]}}",
         "5b6e77ec72f1d114"},
        {"{\"command\":\"node\",\"url\":\"http://x:1/mcp\",\"transport\":\"streamable-http\",\"tools\":{\"include\":[\"z\",\"a\",\"m\"]}}",
         "35b14cbb52e4ca7e"},
        {"{\"command\":\"c\",\"args\":[],\"tools\":{\"include\":[],\"exclude\":[]}}", "ec18b8165f845555"},
        {"{\"command\":\"c\",\"tools\":{\"exclude\":[\"q\",\"p\"],\"include\":[\"s\"]}}", "f5ac012ffd21d60e"},
    };
    for (int i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); ++i) {
        Json cfg = core::json_parse(cases[i].cfg);
        std::string got = tools::config_fingerprint(cfg);
        chk(got == cases[i].want, cases[i].cfg);
    }

    // tools_from_cache_entry: list -> elements; non-list / absent -> empty.
    Json withTools = core::json_parse("{\"tools\":[{\"name\":\"a\"},{\"name\":\"b\"}],\"utility_tools\":[{\"name\":\"u\"}]}");
    std::vector<JsonSP> t = tools::tools_from_cache_entry(withTools);
    chk(t.size() == 2, "tools_present");
    std::vector<JsonSP> u = tools::utility_tools_from_cache_entry(withTools);
    chk(u.size() == 1, "utility_present");

    Json noTools = core::json_parse("{\"command\":\"x\"}");
    chk(tools::tools_from_cache_entry(noTools).size() == 0, "tools_absent");
    chk(tools::utility_tools_from_cache_entry(noTools).size() == 0, "utility_absent");

    Json wrongType = core::json_parse("{\"tools\":\"notalist\"}");
    chk(tools::tools_from_cache_entry(wrongType).size() == 0, "tools_wrongtype");

    if (g_fail == 0) { std::printf("all mcp-schema-cache tests passed\n"); return 0; }
    std::printf("%d failures\n", g_fail);
    return 1;
}
