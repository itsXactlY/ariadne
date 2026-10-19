// Verifies dd::tools path_security + binary_extensions.
#include "dd/tools/path_security.hpp"
#include "dd/tools/binary_extensions.hpp"

#include <filesystem>
#include <iostream>
#include <string>

using namespace dd;
using namespace dd::tools;

static int failures = 0;
#define CHECK(cond, msg)                                   \
    do {                                                   \
        if (!(cond)) {                                     \
            std::cerr << "FAIL: " << msg << "\n";          \
            ++failures;                                    \
        }                                                  \
    } while (0)

int main() {
    using std::filesystem::path;

    // --- validate_within_dir ---
    CHECK(!validate_within_dir("/a/b/c", "/a"), "safe nested path");
    CHECK(!validate_within_dir("/a", "/a"), "equal to root is safe");
    auto esc = validate_within_dir("/a/../etc", "/a");
    CHECK(esc.has_value(), "traversal escapes root");
    if (esc) CHECK(esc->rfind("Path escapes allowed directory:", 0) == 0,
                   "error message prefix");
    auto esc2 = validate_within_dir("/other/x", "/a");
    CHECK(esc2.has_value(), "sibling path escapes root");

    // --- has_traversal_component ---
    CHECK(has_traversal_component("a/../b"), "has .. component");
    CHECK(has_traversal_component("/x/.."), "trailing .. component");
    CHECK(!has_traversal_component("a..b"), "a..b is not traversal");
    CHECK(!has_traversal_component("a/b/c"), "no traversal");

    // --- has_binary_extension ---
    CHECK(has_binary_extension("photo.PNG"), "case-insensitive png");
    CHECK(has_binary_extension("archive.zip"), "zip");
    CHECK(has_binary_extension("x.7z"), "7z");
    CHECK(has_binary_extension("data.sqlite3"), "sqlite3");
    CHECK(!has_binary_extension("file.txt"), "txt not binary");
    CHECK(!has_binary_extension("noext"), "no dot -> not binary");
    CHECK(!has_binary_extension("notes.md"), "md not binary");

    if (failures) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "all tools-security tests passed\n";
    return 0;
}
