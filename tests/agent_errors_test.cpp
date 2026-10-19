// Classification test for dd::agent error hierarchy (errors.py).
#include "dd/agent/errors.hpp"
#include <iostream>
#include <string>

using namespace dd;
using namespace dd::agent;

static int fails = 0;

// catch-tag probe: returns which handler fired for the given throw.
static std::string probe_class(const std::string& tag) {
    std::string caught;
    auto attempt = [&](auto&& f) -> std::string {
        try { f(); return "none"; }
        catch (const std::logic_error&) { return "logic_error"; }
        catch (const std::runtime_error&) { return "runtime_error"; }
        catch (const std::exception&) { return "exception"; }
        catch (...) { return "unknown"; }
    };
    if (tag == "ssl") caught = attempt([] { throw SSLConfigurationError(); });
    else if (tag == "empty") caught = attempt([] { throw EmptyStreamError(); });
    else if (tag == "moa") caught = attempt([] { throw MoAPresetNotFoundError(); });
    return caught;
}

int main() {
    auto ck = [&](const std::string& got, const std::string& want, const char* tag) {
        if (got != want) { ++fails; std::cout << tag << " got=" << got << " want=" << want << "\n"; }
    };
    ck(probe_class("ssl"), "exception", "ssl");
    ck(probe_class("empty"), "runtime_error", "empty");
    ck(probe_class("moa"), "logic_error", "moa");

    std::cout << "errors tests: " << (fails ? "FAIL (" : "PASS (") << fails << " failed)\n";
    return fails ? 1 : 0;
}
