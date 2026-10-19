// Test for dd::agent classification + prompt_caching ports
// (agent/tool_result_classification.py, agent/prompt_caching.py).
#include "dd/agent/classification.hpp"
#include "dd/agent/prompt_caching.hpp"
#include "dd/core/json.hpp"

#include <iostream>
#include <string>
#include <vector>

using namespace dd;
using namespace dd::core;
using namespace dd::agent;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; \
    std::cerr << "FAIL: " << #cond << " @ " << __LINE__ << "\n"; } } while (0)

static bool has_cache_control(const Json& o) {
    return json_get(o, "cache_control") != nullptr;
}
static bool content_block_has_cc(const Json& msg) {
    const Json* c = json_get(msg, "content");
    if (!c || c->type != Json::Arr || c->arr.empty()) return false;
    const Json& block = *c->arr[0];
    return block.type == Json::Obj && has_cache_control(block);
}
static bool content_is_str(const Json& msg, const std::string& s) {
    const Json* c = json_get(msg, "content");
    return c && c->type == Json::Str && c->str == s;
}

int main() {
    // --- tool_result_classification ---
    CHECK(!tool_may_have_side_effect("read_file"));
    CHECK(!tool_may_have_side_effect("web_search"));
    CHECK(tool_may_have_side_effect("write_file"));
    CHECK(tool_may_have_side_effect("patch"));
    CHECK(tool_may_have_side_effect("run_command"));

    CHECK(file_mutation_result_landed("write_file", "{\"bytes_written\": 12}"));
    CHECK(!file_mutation_result_landed("write_file", "{\"error\": \"disk full\"}"));
    CHECK(!file_mutation_result_landed("write_file", "{\"error\": \"\"}"));  // falsy error, no bytes_written
    CHECK(file_mutation_result_landed("patch", "{\"success\": true}"));
    CHECK(!file_mutation_result_landed("patch", "{\"success\": 1}"));   // is True requires bool
    CHECK(!file_mutation_result_landed("patch", "{\"success\": false}"));
    CHECK(!file_mutation_result_landed("read_file", "{}"));             // not file-mutating
    CHECK(!file_mutation_result_landed("write_file", "not json"));      // parse fails

    // --- prompt_caching (system_and_3) ---
    std::vector<Json> msgs;
    msgs.push_back(json_parse("{\"role\":\"system\",\"content\":\"sys prompt\"}"));
    msgs.push_back(json_parse("{\"role\":\"user\",\"content\":\"hello\"}"));
    msgs.push_back(json_parse("{\"role\":\"assistant\",\"content\":\"hi\"}"));
    msgs.push_back(json_parse("{\"role\":\"tool\",\"content\":\"toolout\",\"tool_call_id\":\"x\"}"));
    msgs.push_back(json_parse("{\"role\":\"user\",\"content\":\"again\"}"));

    auto out = apply_anthropic_cache_control(msgs, "5m", false);
    CHECK(out.size() == 5);

    // system (idx0): content wrapped, block carries cache_control
    CHECK(content_block_has_cc(out[0]));
    // idx1 user NOT in last-3 -> content unchanged
    CHECK(content_is_str(out[1], "hello"));
    // idx2 assistant -> wrapped with cache_control
    CHECK(content_block_has_cc(out[2]));
    // idx3 tool role, native=false -> NO cache_control, content unchanged
    CHECK(!has_cache_control(out[3]));
    CHECK(content_is_str(out[3], "toolout"));
    // idx4 user -> wrapped with cache_control
    CHECK(content_block_has_cc(out[4]));

    // native_anthropic=true -> tool message DOES get top-level cache_control
    auto out2 = apply_anthropic_cache_control(msgs, "5m", true);
    CHECK(has_cache_control(out2[3]));

    // ttl=1h marker carries ttl (verify via a system-only conversation)
    std::vector<Json> one;
    one.push_back(json_parse("{\"role\":\"system\",\"content\":\"s\"}"));
    auto out3 = apply_anthropic_cache_control(one, "1h", false);
    CHECK(content_block_has_cc(out3[0]));

    if (failures == 0) {
        std::cout << "all agent-misc tests passed\n";
        return 0;
    }
    std::cout << failures << " agent-misc test failures\n";
    return 1;
}
