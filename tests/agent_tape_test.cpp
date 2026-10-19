// Test for dd::agent payload_tape port (agent/payload_tape.py).
#include "dd/agent/payload_tape.hpp"
#include "dd/core/json.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace dd;
using namespace dd::core;
using namespace dd::agent;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; \
    std::cerr << "FAIL: " << #cond << " @ " << __LINE__ << "\n"; } } while (0)

static JsonSP make_msg(const std::string& role) {
    Json m = core::json_obj();
    core::obj_set(m, "role", core::json_str(role));
    return std::make_shared<Json>(std::move(m));
}
static Json make_rendered(const std::string& tag) {
    Json r = core::json_obj();
    core::obj_set(r, "tag", core::json_str(tag));
    return r;
}

int main() {
    auto m1 = make_msg("system"), m2 = make_msg("user"), m3 = make_msg("assistant");
    auto m1copy = make_msg("system");  // equal value, different identity

    PayloadTape tape;
    CHECK(!static_cast<bool>(tape));
    CHECK(!tape.generation.has_value());

    tape.start({m1, m2}, {make_rendered("r0")}, 1);
    CHECK(tape.blocks.size() == 1);
    CHECK(tape.generation.has_value() && *tape.generation == 1);
    CHECK(tape.first_source() == m1);

    // Longest matching tape prefix: block0 covers m1,m2 -> (1, 2).
    auto mm = tape.matched_blocks({m1, m2, m3}, 1);
    CHECK(mm.first == 1 && mm.second == 2);

    // Generation mismatch -> no match.
    auto mm2 = tape.matched_blocks({m1, m2}, 2);
    CHECK(mm2.first == 0 && mm2.second == 0);

    // Identity, not equality: a copy at pos 0 breaks the match.
    auto mm3 = tape.matched_blocks({m1copy, m2}, 1);
    CHECK(mm3.first == 0 && mm3.second == 0);

    // View shorter than the block -> no match.
    auto mm4 = tape.matched_blocks({m1}, 1);
    CHECK(mm4.first == 0 && mm4.second == 0);

    tape.append(m3, {make_rendered("r1")});
    CHECK(tape.blocks.size() == 2);

    auto pl = tape.payload();
    CHECK(pl.size() == 2);
    CHECK(pl.size() == 2 &&
          json_get(pl[0], "tag")->str == "r0" &&
          json_get(pl[1], "tag")->str == "r1");

    tape.truncate(1);
    CHECK(tape.blocks.size() == 1);
    CHECK(static_cast<bool>(tape));

    tape.reset();
    CHECK(!static_cast<bool>(tape));
    CHECK(!tape.generation.has_value());

    if (failures == 0) { std::cout << "all agent-tape tests passed\n"; return 0; }
    std::cout << failures << " agent-tape test failures\n";
    return 1;
}
