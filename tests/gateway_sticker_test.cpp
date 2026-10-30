// Verifies dd::gateway::sticker_cache against gateway/sticker_cache.py.
#include "dd/gateway/sticker_cache.hpp"
#include "dd/core/home.hpp"
#include "dd/core/json.hpp"

#include <filesystem>
#include <iostream>
#include <string>

using namespace dd;
using namespace dd::core;
using namespace dd::gateway;

static int failures = 0;
#define CHECK(cond, msg)                                   \
    do {                                                   \
        if (!(cond)) {                                     \
            std::cerr << "FAIL: " << msg << "\n";          \
            ++failures;                                    \
        }                                                  \
    } while (0)

int main() {
    // Isolate the cache under a temp home so we never touch the real one.
    set_ariadne_home_override("/tmp/dd_sticker_test");
    std::error_code ec;
    std::filesystem::remove_all("/tmp/dd_sticker_test", ec);

    // 1. Unknown id -> nullopt (empty cache).
    CHECK(!get_cached_description("nope"), "unknown id -> nullopt");

    // 2. Cache a sticker with emoji + set_name, then read it back.
    cache_sticker_description("st-1", "A cat waving", "\xF0\x9F\x98\x80", "MyPack");
    auto e = get_cached_description("st-1");
    CHECK(e.has_value(), "cached id found");
    if (e) {
        const Json* d = json_get(*e, "description");
        const Json* em = json_get(*e, "emoji");
        const Json* sn = json_get(*e, "set_name");
        CHECK(d && d->type == Json::Str && d->str == "A cat waving", "description round-trips");
        CHECK(em && em->type == Json::Str && em->str == "\xF0\x9F\x98\x80", "emoji round-trips");
        CHECK(sn && sn->type == Json::Str && sn->str == "MyPack", "set_name round-trips");
    }

    // 3. Injection string with set_name + emoji.
    std::string inj = build_sticker_injection("A cat waving", "\xF0\x9F\x98\x80", "MyPack");
    CHECK(inj == "[The user sent a sticker \xF0\x9F\x98\x80 from \"MyPack\"~ It shows: \"A cat waving\" (=^.w.^=)]",
          "injection with set_name+emoji");

    // 4. Injection with only emoji (no set_name) -> context is just " emoji".
    std::string inj2 = build_sticker_injection("Hi", "\xF0\x9F\x98\x80", "");
    CHECK(inj2 == "[The user sent a sticker \xF0\x9F\x98\x80~ It shows: \"Hi\" (=^.w.^=)]",
          "injection with only emoji");

    // 5. Injection with neither -> no context.
    std::string inj3 = build_sticker_injection("Hi", "", "");
    CHECK(inj3 == "[The user sent a sticker~ It shows: \"Hi\" (=^.w.^=)]", "injection with no context");

    // 6. Animated injection is fixed regardless of emoji.
    CHECK(build_animated_sticker_injection("x") ==
              "[The user sent an animated sticker~ I can't see animated ones yet]",
          "animated injection fixed");

    std::filesystem::remove_all("/tmp/dd_sticker_test", ec);

    if (failures) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "all sticker-cache tests passed\n";
    return 0;
}
