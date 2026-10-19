// Tests for dd::core SHA-1 + UUID5 primitives.
#include "dd/core/sha1.hpp"

#include <cstdio>
#include <iostream>
#include <string>

using namespace dd::core;

static int failures = 0;

static void check(const std::string& got, const std::string& want, const char* what) {
    if (got != want) {
        std::cerr << "FAIL " << what << ": got=" << got << " want=" << want << "\n";
        ++failures;
    }
}

int main() {
    // Ground truth from sha1sum / openssl / hashlib.
    check(sha1_hex("abc"), "a9993e364706816aba3e25717850c26c9cd0d89d", "sha1(abc)");
    check(sha1_hex(""), "da39a3ee5e6b4b0d3255bfef95601890afd80709", "sha1(empty)");
    check(sha1_hex("a"), "86f7e437faa5a7fce15d1ddcb9eaeaea377667b8", "sha1(a)");
    check(sha1_hex("The quick brown fox jumps over the lazy dog"),
          "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12", "sha1(fox)");

    // uuid5(NAMESPACE_URL, name) ground truth from Python uuid.
    check(uuid5_url_hex("camofox-user:/home/test/.daedalus/browser_auth/camofox"),
          "6239b11c43c4596185ea6b9ef6b46dee", "uuid5(camofox)");
    check(uuid5_url_hex(""), "1b4db7eb40575ddf91e036dec72071f5", "uuid5(empty)");

    if (failures == 0) {
        std::cout << "all sha1 tests passed\n";
        return 0;
    }
    std::cout << failures << " sha1 test(s) failed\n";
    return 1;
}
