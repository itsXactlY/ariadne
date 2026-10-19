// Differential test for dd::agent::backoff_delay (retry_utils.py).
#include "dd/agent/retry_utils.hpp"
#include <cmath>
#include <iostream>

using namespace dd;
using namespace dd::agent;

static int fails = 0;
static void ck(double got, double want, const char* tag) {
    if (std::fabs(got - want) > 1e-9) { ++fails; std::cout << tag << " got=" << got
                                        << " want=" << want << "\n"; }
}

int main() {
    ck(backoff_delay(1, 5.0, 120.0), 5.0, "a1");
    ck(backoff_delay(2, 5.0, 120.0), 10.0, "a2");
    ck(backoff_delay(4, 5.0, 120.0), 40.0, "a4");
    ck(backoff_delay(7, 5.0, 120.0), 120.0, "a7cap");
    ck(backoff_delay(0, 5.0, 120.0), 5.0, "a0");
    ck(backoff_delay(-3, 5.0, 120.0), 5.0, "aneg");
    ck(backoff_delay(64, 5.0, 120.0), 120.0, "a64");
    ck(backoff_delay(63, 5.0, 120.0), 120.0, "a63");
    ck(backoff_delay(3, 0.0, 120.0), 120.0, "bzero");
    ck(backoff_delay(3, -2.0, 120.0), 120.0, "bneg");
    ck(backoff_delay(10, 5.0, 120.0), 120.0, "a10cap");
    ck(backoff_delay(5, 5.0, 120.0), 80.0, "a5");
    ck(backoff_delay(6, 5.0, 120.0), 120.0, "a6cap");

    std::cout << "retry_utils tests: " << (fails ? "FAIL (" : "PASS (") << fails << " failed)\n";
    return fails ? 1 : 0;
}
