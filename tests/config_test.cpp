// tests/config_test.cpp — verify the YAML config loader against tests/fixtures/config.yaml (DD_FIXTURES overrides the folder).
#include "dd/core/config.hpp"
#include "dd/core/json.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
using namespace dd;
using namespace dd::core;
using dd::core::Json;

static int failures = 0;

#define CHECK(cond, what)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            ++failures;                                               \
            std::cerr << "FAIL: " << what << " @ " << __LINE__ << "\n"; \
        }                                                             \
    } while (0)

int main() {
    const char* fx = std::getenv("DD_FIXTURES");   // the test runs from the repo root
    Config c = load_config(std::string(fx && *fx ? fx : "tests/fixtures") + "/config.yaml");

    // version
    CHECK(c.get_int("_config_version") == 16, "_config_version==16");

    // model map + nested scalars
    CHECK(c.get_map("model") != nullptr, "model is a map");
    CHECK(c.get_string("model.name") == "example-model-q4", "model.name");
    CHECK(c.get_string("model.provider") == "custom", "model.provider==custom");
    CHECK(c.get_string("model.base_url") == "http://127.0.0.1:8080/v1", "model.base_url");

    // empty inline collections
    const Json* toolsets = c.get_list("toolsets");
    CHECK(toolsets && toolsets->arr.size() == 0, "toolsets empty list");
    const Json* fb = c.get_list("fallback_providers");
    CHECK(fb && fb->arr.size() == 0, "fallback_providers empty list");
    CHECK(c.get_map("honcho") != nullptr, "honcho empty map");

    // agent scalars
    CHECK(c.get_int("agent.max_turns") == 400, "agent.max_turns==400");
    CHECK(c.get_bool("agent.tool_use_enforcement") == true, "agent.tool_use_enforcement true");

    // nested double
    CHECK(c.get_double("compression.watermarks.high") == 0.88, "compression.watermarks.high==0.88");
    CHECK(c.get_double("compression.threshold") == 0.83, "compression.threshold==0.83");

    // block list of 4
    const Json* always = c.get_list("tool_search.always_load");
    CHECK(always && always->arr.size() == 4, "tool_search.always_load size 4");

    // skills.external_dirs block list of 1
    const Json* ext = c.get_list("skills.external_dirs");
    CHECK(ext && ext->arr.size() == 1, "skills.external_dirs size 1");
    CHECK(ext && ext->arr.size() == 1 && ext->arr[0]->str == "/opt/skills/extra",
          "skills.external_dirs[0]");

    // nested map under mcp_servers
    CHECK(c.get_map("mcp_servers.memory.tools") != nullptr, "mcp_servers.memory.tools map");

    // multi-line double-quoted description with \u escapes + line continuation
    std::string search = c.get_string("mcp_servers.search.description");
    CHECK(search.find("SEARCH") != std::string::npos, "search desc has SEARCH");
    CHECK(search.find("arXiv") != std::string::npos, "search desc has arXiv (after the line continuation)");
    CHECK(search.find("arXiv, reports") != std::string::npos, "line continuation joins without a space");
    CHECK(search.find("\xE2\x80\x94") != std::string::npos, "search desc has em-dash");

    // single-line \u description
    std::string quant = c.get_string("mcp_servers.quant.description");
    CHECK(quant.find("QUANT") != std::string::npos, "quant desc has QUANT");
    CHECK(quant.find("\xE2\x80\x94") != std::string::npos, "quant desc has em-dash");

    // flow collections, per-tool switch, enabled flags
    CHECK(c.get_string("agent.system_prompt").empty() && c.get_map("agent") != nullptr, "empty system prompt");
    const Json* disabled = c.get_list("skills.disabled");
    CHECK(disabled && disabled->arr.size() == 3 && disabled->arr[1]->str == "beta" && disabled->arr[2]->str == "gamma", "flow list with mixed quoting");
    CHECK(c.get_int("providers.custom.options.timeout") == 30, "flow map value");
    CHECK(c.get_bool("mcp_servers.memory.enabled"), "mcp server enabled");
    CHECK(!c.get_bool("mcp_servers.quant.enabled"), "mcp server disabled");
    CHECK(c.get_string("mcp_servers.memory.tools.memory_prune.enabled") == "false", "per-tool switch");
    CHECK(c.get_string("model.api_key").empty(), "empty quoted scalar");
    CHECK(c.get_string("model.name") == "example-model-q4", "trailing comment after a plain scalar");

    // misc scalars
    CHECK(c.get_string("terminal.backend") == "local", "terminal.backend==local");
    CHECK(c.get_string("timezone") == "", "timezone empty");

    // a couple of top-level maps exist
    CHECK(c.get_map("providers") != nullptr, "providers map");
    CHECK(c.get_map("skills") != nullptr, "skills map");
    CHECK(c.has("model"), "has model");
    CHECK(!c.has("model.does_not_exist"), "no bogus key");

    // trailing comments: after quoted values, on a key that opens a block
    {
        Config t(parse_yaml(
            "model:\n"
            "  api_key: \"\"        # none needed\n"
            "  name: 'it''s'      # quoted, with an escaped quote\n"
            "  base_url: http://h:1/v1#frag   # '#' inside a word stays\n"
            "  note: it's fine    # an apostrophe mid-word is text\n"
            "  color: \"#B026FF\"   # '#' inside quotes stays\n"
            "mcp_servers:        # a block follows\n"
            "  mz:\n"
            "    tools:          # per-tool switch\n"
            "      prune: {enabled: false}  # flow map\n"
            "deny: []            # empty list\n"
            "after: 1\n"));
        CHECK(t.get_string("model.api_key") == "", "comment after \"\"");
        CHECK(t.get_string("model.name") == "it's", "comment after ''");
        CHECK(t.get_string("model.base_url") == "http://h:1/v1#frag", "# inside a word");
        CHECK(t.get_string("model.note") == "it's fine", "apostrophe mid-word");
        CHECK(t.get_string("model.color") == "#B026FF", "# inside quotes");
        CHECK(t.get_string("mcp_servers.mz.tools.prune.enabled") == "false", "comment on a block key");
        const Json* deny = t.get_list("deny");
        CHECK(deny && deny->type == Json::Arr && deny->arr.empty(), "comment after []");
        CHECK(t.get_int("after") == 1, "keys after the block survive");
    }

    if (failures == 0) {
        std::cout << "all config tests passed\n";
        return 0;
    }
    std::cout << failures << " config test failures\n";
    return 1;
}
