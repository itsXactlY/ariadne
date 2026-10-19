// Differential test for dd::gateway::select_handlers (hooks.py emit selection).
#include "dd/gateway/hooks.hpp"
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace dd;
using namespace dd::gateway;

int fails = 0;

static void eq(const std::vector<std::string>& got, const std::vector<std::string>& want, const char* tag) {
    if (got != want) {
        ++fails;
        std::cout << tag << " got=[";
        for (size_t i = 0; i < got.size(); ++i) { if (i) std::cout << ","; std::cout << got[i]; }
        std::cout << "] want=[";
        for (size_t i = 0; i < want.size(); ++i) { if (i) std::cout << ","; std::cout << want[i]; }
        std::cout << "]\n";
    }
}

int main() {
    // command:* handler fires for any command:...
    std::map<std::string, std::vector<std::string>> h1{{"command:*", {"ch"}}};
    eq(select_handlers(h1, "command:foo"), {"ch"}, "wild1");
    eq(select_handlers(h1, "command:new"), {"ch"}, "wild2");
    eq(select_handlers(h1, "agent:start"), {}, "wild3");  // base "agent" not in map

    // base-only handler does NOT fire for agent:start
    std::map<std::string, std::vector<std::string>> h2{{"agent", {"a"}}};
    eq(select_handlers(h2, "agent:start"), {}, "baseonly");
    eq(select_handlers(h2, "agent"), {"a"}, "exactonly");

    // exact + wildcard both registered -> exact first, then wildcard
    std::map<std::string, std::vector<std::string>> h3{
        {"agent:start", {"E"}}, {"agent:*", {"W1", "W2"}}};
    eq(select_handlers(h3, "agent:start"), {"E", "W1", "W2"}, "both");
    eq(select_handlers(h3, "agent:step"), {"W1", "W2"}, "wildonly");

    // multiple exact handlers preserved in order
    std::map<std::string, std::vector<std::string>> h4{
        {"gateway:startup", {"b1", "b2"}}, {"gateway:*", {"w"}}};
    eq(select_handlers(h4, "gateway:startup"), {"b1", "b2", "w"}, "multi");

    // no colon -> only exact, never a wildcard
    std::map<std::string, std::vector<std::string>> h5{{"startup", {"s"}}, {"startup:*", {"z"}}};
    eq(select_handlers(h5, "startup"), {"s"}, "nocolon");

    // empty handler map
    std::map<std::string, std::vector<std::string>> empty;
    eq(select_handlers(empty, "command:x"), {}, "empty");

    std::cout << "hooks tests: " << (fails ? "FAIL (" : "PASS (") << fails << " failed)\n";
    return fails ? 1 : 0;
}
