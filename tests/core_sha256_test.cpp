// Ground-truth tests for the SHA-256 core primitive, on every kernel the CPU has (scalar, SHA-NI).
#include "dd/core/sha256.hpp"
#include <atomic>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace dd::core;

static int g_fail = 0;
static void chk(bool ok, const std::string& label) {
    if (!ok) { ++g_fail; std::printf("FAIL %s\n", label.c_str()); }
}

// FIPS vectors, the 56-byte padding boundary, UTF-8: the answers every kernel must give
static void known_answers(const std::string& tag) {
    chk(sha256_hex("abc") ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        tag + " abc");
    chk(sha256_hex("") ==
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        tag + " empty");
    chk(sha256_hex("The quick brown fox jumps over the lazy dog") ==
            "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592",
        tag + " fox");
    chk(sha256_hex(std::string(1000000, 'a')) ==
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
        tag + " million_a");
    // 56-byte boundary: exactly one block of padding.
    chk(sha256_hex(std::string(55, 'a')) ==
            "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318",
        tag + " 55a");
    chk(sha256_hex(std::string(56, 'a')) ==
            "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a",
        tag + " 56a");
    chk(sha256_hex(std::string(64, 'a')) ==
            "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb",
        tag + " 64a");
    chk(sha256_hex("\xc3\xa9") ==
            "4a99557e4033c3539de2eb65472017cad5f9557f7a0625a09f1c3f6e2ba69c4c",
        tag + " utf8_eacute");
    chk(sha256_hex("\xe6\x97\xa5\xe6\x9c\xac") ==
            "cf2abf0c5be326cb922a70f8163f91079c4d9aa8655c60ead89ad545c9de2e92",
        tag + " utf8_cjk");
}

// digests of n x 'a' from coreutils sha256sum, at the lengths where the padding changes shape
static void padding_boundaries(const std::string& tag) {
    struct Case { size_t n; const char* digest; };
    static const Case cases[] = {
        {0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {1, "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb"},
        {55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
        {56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
        {63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
        {64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
        {65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"},
        {119, "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb"},
        {120, "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c"},
        {127, "c57e9278af78fa3cab38667bef4ce29d783787a2f731d4e12200270f0c32320a"},
        {128, "6836cf13bac400e9105071cd6af47084dfacad4e5e302c94bfed24e013afb73e"},
        {129, "c12cb024a2e5551cca0e08fce8f1c5e314555cc3fef6329ee994a3db752166ae"},
        {191, "311834ddbafe20677dddbf9f4beb2b5b0b9e6ee97dddb70fce4e8f62c1b7518d"},
        {192, "7cee24628d290c16183532716cc5a8a889bc951b4b0a1507c32b8e29cee01052"},
    };
    for (const Case& c : cases) chk(sha256_hex(std::string(c.n, 'a')) == c.digest, tag + " padding " + std::to_string(c.n));
}

static std::string random_bytes(std::mt19937& rng, size_t n) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>(rng());
    return s;
}

int main() {
    const std::string best = sha256::level();
    std::printf("best kernel on this CPU: %s\n", best.c_str());

    // every kernel the CPU has: the known answers
    for (const char* level : {"scalar", "shani"}) {
        if (!sha256::force(level)) { std::printf("skip %s (cpu lacks it)\n", level); continue; }
        chk(std::string(sha256::level()) == level, std::string("force ") + level);
        known_answers(level);
        padding_boundaries(level);
    }
    chk(!sha256::force("avx9"), "an unknown kernel is refused");

    // scalar against SHA-NI on random input: every length to 600, then a few large buffers
    std::mt19937 rng(20261022);
    std::vector<std::string> inputs;
    for (size_t n = 0; n <= 600; ++n) inputs.push_back(random_bytes(rng, n));
    for (size_t n : {size_t{1} << 20, (size_t{1} << 20) + 3, (size_t{4} << 20) - 1, size_t{8} << 20}) inputs.push_back(random_bytes(rng, n));
    std::vector<std::string> by_scalar, by_shani;
    sha256::force("scalar");
    for (const auto& s : inputs) by_scalar.push_back(sha256_hex(s));
    if (sha256::force("shani")) {
        for (const auto& s : inputs) by_shani.push_back(sha256_hex(s));
        for (size_t i = 0; i < inputs.size(); ++i)
            chk(by_scalar[i] == by_shani[i], "scalar and shani agree on " + std::to_string(inputs[i].size()) + " random bytes");

        // several threads at once, the kernel chosen once and shared: no state to trip over
        std::atomic<int> bad{0};
        std::vector<std::thread> pool;
        for (int t = 0; t < 4; ++t)
            pool.emplace_back([&, t] {
                for (size_t i = static_cast<size_t>(t); i < inputs.size(); i += 4)
                    if (sha256_hex(inputs[i]) != by_scalar[i]) ++bad;
            });
        for (auto& th : pool) th.join();
        chk(bad == 0, "four threads hashing at once agree with scalar");
    } else {
        std::printf("skip shani comparison (cpu lacks it)\n");
    }

    sha256::force(best.c_str());
    chk(best == sha256::level(), "the best kernel is restored");

    if (g_fail == 0) { std::printf("all sha256 tests passed\n"); return 0; }
    std::printf("%d failures\n", g_fail);
    return 1;
}
