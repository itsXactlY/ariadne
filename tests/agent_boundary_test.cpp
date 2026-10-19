// Test for dd::agent prompt_cache_boundary port (agent/prompt_cache_boundary.py).
#include "dd/agent/prompt_cache_boundary.hpp"

#include <iostream>
#include <optional>
#include <string>

using namespace dd;
using namespace dd::agent;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; \
    std::cerr << "FAIL: " << #cond << " @ " << __LINE__ << "\n"; } } while (0)

int main() {
    clear_stable_prefixes();

    register_stable_prefix("AAA");
    register_stable_prefix("AAAA");

    // Longest proper prefix wins.
    auto hit = find_stable_prefix("AAAAA");
    CHECK(hit.has_value() && *hit == "AAAA");

    // Not a proper prefix (equal length) -> no hit.
    auto eq = find_stable_prefix("AAA");
    CHECK(!eq.has_value());

    // No registered prefix matches.
    auto miss = find_stable_prefix("AAB");
    CHECK(!miss.has_value());

    // Empty prefix is a no-op register.
    register_stable_prefix("");

    // LRU eviction: 33 distinct prefixes -> capped at 32 (oldest dropped).
    clear_stable_prefixes();
    for (int i = 0; i < 33; ++i) {
        register_stable_prefix("p" + std::to_string(i));
    }
    // "p0" was the oldest and must have been evicted.
    auto evicted = find_stable_prefix("p0-tail");
    CHECK(!evicted.has_value());
    // "p32" is newest and present.
    auto newest = find_stable_prefix("p32-tail");
    CHECK(newest.has_value() && *newest == "p32");

    // MAX_CHARS eviction: two 3MB prefixes -> oldest dropped, only newest kept.
    clear_stable_prefixes();
    std::string big1(3 * 1024 * 1024, 'a');
    std::string big2(3 * 1024 * 1024, 'b');
    register_stable_prefix(big1);
    register_stable_prefix(big2);
    auto gone = find_stable_prefix(big1 + "z");
    CHECK(!gone.has_value());
    auto kept = find_stable_prefix(big2 + "z");
    CHECK(kept.has_value() && *kept == big2);

    if (failures == 0) {
        std::cout << "all agent-boundary tests passed\n";
        return 0;
    }
    std::cout << failures << " agent-boundary test failures\n";
    return 1;
}
