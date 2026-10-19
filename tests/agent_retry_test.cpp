// Test for dd::agent retry_utils + errors ports (agent/retry_utils.py, agent/errors.py).
#include "dd/agent/retry.hpp"
#include "dd/agent/errors.hpp"

#include <cmath>
#include <iostream>

using namespace dd;
using namespace dd::agent;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; \
    std::cerr << "FAIL: " << #cond << " @ " << __LINE__ << "\n"; } } while (0)

// delay(attempt, base, max) = min(base * 2^max(0,attempt-1), max) with the exponent>=63 / base<=0 guards.
static double expected_delay(int attempt, double base, double maxd) {
    int exponent = attempt - 1;
    if (exponent < 0) exponent = 0;
    if (exponent >= 63 || base <= 0.0) return maxd;
    double d = base * std::pow(2.0, exponent);
    return d < maxd ? d : maxd;
}

int main() {
    // --- jittered_backoff bounds ---
    struct Case { int attempt; double base; double maxd; double ratio; };
    Case cases[] = {
        {1, 5.0, 120.0, 0.5},
        {10, 5.0, 120.0, 0.5},   // 5*512=2560 -> capped 120
        {0, 5.0, 120.0, 0.5},    // exponent clamped to 0
        {1, 0.0, 120.0, 0.5},    // base<=0 -> max_delay
        {100, 5.0, 120.0, 0.5},  // exponent>=63 -> max_delay
        {3, 2.0, 1000.0, 0.25},  // 2*4=8
    };
    for (const auto& c : cases) {
        double delay = expected_delay(c.attempt, c.base, c.maxd);
        double r = jittered_backoff(c.attempt, c.base, c.maxd, c.ratio);
        CHECK(r >= delay);
        CHECK(r <= delay + c.ratio * delay + 1e-9);
    }

    // ratio=0 -> jitter is exactly 0, result == delay.
    CHECK(jittered_backoff(1, 5.0, 120.0, 0.0) == 5.0);
    CHECK(jittered_backoff(10, 5.0, 120.0, 0.0) == 120.0);

    // --- errors hierarchy: catch by class + by std base ---
    bool caught_ssl = false, caught_empty = false, caught_moa = false;
    try { throw SSLConfigurationError("bad cert bundle"); }
    catch (const SSLConfigurationError& e) { caught_ssl = (std::string(e.what()) == "bad cert bundle"); }
    CHECK(caught_ssl);

    try { throw EmptyStreamError("no response"); }
    catch (const std::runtime_error& e) { caught_empty = (std::string(e.what()) == "no response"); }
    CHECK(caught_empty);

    try { throw MoAPresetNotFoundError("preset gone"); }
    catch (const std::invalid_argument& e) { caught_moa = (std::string(e.what()) == "preset gone"); }
    CHECK(caught_moa);

    if (failures == 0) {
        std::cout << "all agent-retry tests passed\n";
        return 0;
    }
    std::cout << failures << " agent-retry test failures\n";
    return 1;
}
