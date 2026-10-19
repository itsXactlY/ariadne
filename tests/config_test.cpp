// tests/config_test.cpp — verify the YAML config loader against the real config.yaml.
#include "dd/core/config.hpp"
#include "dd/core/json.hpp"

#include <iostream>
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
    Config c = load_config("/home/alca/.daedalus/config.yaml");

    // version
    CHECK(c.get_int("_config_version") == 16, "_config_version==16");

    // model map + nested scalars
    CHECK(c.get_map("model") != nullptr, "model is a map");
    CHECK(c.get_string("model.name") == "qwen3.8-flash-next-coder-iq1_m", "model.name");
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
    CHECK(ext && ext->arr.size() == 1 && ext->arr[0]->str == "/home/alca/.daedalus/skills-maze",
          "skills.external_dirs[0]");

    // nested map under mcp_servers
    CHECK(c.get_map("mcp_servers.mazemaker.tools") != nullptr, "mcp_servers.mazemaker.tools map");

    // multi-line double-quoted description with \u escapes + line continuation
    std::string pulse = c.get_string("mcp_servers.pulse.description");
    CHECK(pulse.find("PULSE") != std::string::npos, "pulse desc has PULSE");
    CHECK(pulse.find("arXiv") != std::string::npos, "pulse desc has arXiv");
    CHECK(pulse.find("\xE2\x80\x94") != std::string::npos, "pulse desc has em-dash");

    // single-line \u description
    std::string btq = c.get_string("mcp_servers.btquant.description");
    CHECK(btq.find("BTQuant") != std::string::npos, "btquant desc has BTQuant");
    CHECK(btq.find("\xE2\x80\x94") != std::string::npos, "btquant desc has em-dash");

    // misc scalars
    CHECK(c.get_string("terminal.backend") == "local", "terminal.backend==local");
    CHECK(c.get_string("timezone") == "", "timezone empty");

    // a couple of top-level maps exist
    CHECK(c.get_map("providers") != nullptr, "providers map");
    CHECK(c.get_map("skills") != nullptr, "skills map");
    CHECK(c.has("model"), "has model");
    CHECK(!c.has("model.does_not_exist"), "no bogus key");

    if (failures == 0) {
        std::cout << "all config tests passed\n";
        return 0;
    }
    std::cout << failures << " config test failures\n";
    return 1;
}
