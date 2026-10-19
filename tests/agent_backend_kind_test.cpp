// Differential test for dd::agent::parse_single_sequence_override.
#include "dd/agent/backend_kind.hpp"
#include <iostream>
#include <optional>
#include <string>

using namespace dd;
using namespace dd::agent;

static int fails = 0;
static void ck(std::optional<bool> got, const std::string& want, const char* tag) {
    std::string g = got ? (*got ? "T" : "F") : "-";
    if (g != want) { ++fails; std::cout << tag << " got=" << g << " want=" << want << "\n"; }
}

int main() {
    ck(parse_single_sequence_override(""), "-", "empty");
    ck(parse_single_sequence_override("  "), "-", "spaces");
    ck(parse_single_sequence_override("1"), "T", "one");
    ck(parse_single_sequence_override("0"), "F", "zero");
    ck(parse_single_sequence_override(" 1 "), "T", "sp1");
    ck(parse_single_sequence_override("TRUE"), "T", "TRUE");
    ck(parse_single_sequence_override("Yes"), "T", "Yes");
    ck(parse_single_sequence_override(" NO "), "F", "NO");
    ck(parse_single_sequence_override("1.0"), "-", "1.0");
    ck(parse_single_sequence_override("2"), "-", "two");
    ck(parse_single_sequence_override("true1"), "-", "true1");
    ck(parse_single_sequence_override(" on "), "-", "on");
    ck(parse_single_sequence_override("true\n"), "T", "truenl");

    std::cout << "backend_kind tests: " << (fails ? "FAIL (" : "PASS (") << fails << " failed)\n";
    return fails ? 1 : 0;
}
