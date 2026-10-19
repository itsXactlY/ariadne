// Differential test for dd::agent::skip_ssl_guard_enabled (ssl_guard.py).
#include "dd/agent/ssl_guard.hpp"
#include <iostream>
#include <string>

using namespace dd;
using namespace dd::agent;

static int fails = 0;
static void ck(bool got, bool want, const char* tag) {
    if (got != want) { ++fails; std::cout << tag << " got=" << (got ? "T" : "F")
                        << " want=" << (want ? "T" : "F") << "\n"; }
}

int main() {
    ck(skip_ssl_guard_enabled(""), false, "empty");
    ck(skip_ssl_guard_enabled("  "), false, "spaces");
    ck(skip_ssl_guard_enabled("1"), true, "one");
    ck(skip_ssl_guard_enabled("0"), false, "zero");
    ck(skip_ssl_guard_enabled(" YES "), true, "yes");
    ck(skip_ssl_guard_enabled("On"), true, "On");
    ck(skip_ssl_guard_enabled("true"), true, "true");
    ck(skip_ssl_guard_enabled("true1"), false, "true1");
    ck(skip_ssl_guard_enabled("on\n"), true, "truenl");
    ck(skip_ssl_guard_enabled("1 "), true, "one-sp");
    ck(skip_ssl_guard_enabled("on "), true, "on-sp");

    std::cout << "ssl_guard tests: " << (fails ? "FAIL (" : "PASS (") << fails << " failed)\n";
    return fails ? 1 : 0;
}
