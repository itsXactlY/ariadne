// Ground-truth tests for the SHA-256 core primitive.
#include "dd/core/sha256.hpp"
#include <cstdio>
#include <string>

using namespace dd::core;

static int g_fail = 0;
static void chk(bool ok, const std::string& label) {
    if (!ok) { ++g_fail; std::printf("FAIL %s\n", label.c_str()); }
}

int main() {
    chk(sha256_hex("abc") ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "abc");
    chk(sha256_hex("") ==
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "empty");
    chk(sha256_hex("The quick brown fox jumps over the lazy dog") ==
            "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592",
        "fox");
    chk(sha256_hex(std::string(1000000, 'a')) ==
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
        "million_a");
    // 56-byte boundary: exactly one block of padding.
    chk(sha256_hex(std::string(55, 'a')) ==
            "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318",
        "55a");
    chk(sha256_hex(std::string(56, 'a')) ==
            "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a",
        "56a");
    chk(sha256_hex(std::string(64, 'a')) ==
            "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb",
        "64a");
    chk(sha256_hex("\xc3\xa9") ==
            "4a99557e4033c3539de2eb65472017cad5f9557f7a0625a09f1c3f6e2ba69c4c",
        "utf8_eacute");
    chk(sha256_hex("\xe6\x97\xa5\xe6\x9c\xac") ==
            "cf2abf0c5be326cb922a70f8163f91079c4d9aa8655c60ead89ad545c9de2e92",
        "utf8_cjk");

    if (g_fail == 0) { std::printf("all sha256 tests passed\n"); return 0; }
    std::printf("%d failures\n", g_fail);
    return 1;
}
