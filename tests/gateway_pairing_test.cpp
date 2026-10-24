// Verifies dd::gateway::PairingStore against gateway/pairing.py.
#include "dd/gateway/pairing.hpp"
#include "dd/core/home.hpp"
#include "dd/core/json.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace dd;
using namespace dd::core;
using namespace dd::gateway;

static int failures = 0;
#define CHECK(cond, msg)                                   \
    do {                                                   \
        if (!(cond)) {                                     \
            std::cerr << "FAIL: " << msg << "\n";          \
            ++failures;                                    \
        }                                                  \
    } while (0)

static bool code_valid(const std::string& code) {
    const std::string A = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    if (code.size() != 8) return false;
    for (char c : code)
        if (A.find(c) == std::string::npos) return false;
    return true;
}

// Chi-square of `draws` characters over `alphabet` against the uniform expectation.
static double chi_square(const std::string& alphabet, int draws) {
    std::vector<long> seen(alphabet.size(), 0);
    for (int done = 0; done < draws;) {
        const int n = std::min(draws - done, 4096);
        const std::string s = pairing_random_code(alphabet, n);
        for (char c : s) ++seen[alphabet.find(c)];
        done += n;
    }
    const double expect = static_cast<double>(draws) / static_cast<double>(alphabet.size());
    double x = 0;
    for (long c : seen) x += (c - expect) * (c - expect) / expect;
    return x;
}

int main() {
    // The scratch home lives beside the build, never in /tmp (a shared RAM tmpfs).
    const char* t = std::getenv("DD_TEST_TMP");
    const std::filesystem::path dir =
        std::filesystem::absolute(std::filesystem::path(t && *t ? t : "build") / ("dd_pairing_test-" + std::to_string(::getpid())));
    set_daedalus_home_override(dir.string());
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    // --- the code generator: OS randomness, no bias ---
    {
        const std::string A = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        CHECK(pairing_random_code(A, 8).size() == 8, "random code length");
        CHECK(pairing_random_code("", 8).empty() && pairing_random_code(A, 0).empty(), "empty alphabet or length gives nothing");
        CHECK(pairing_random_code(std::string(300, 'x'), 4).empty(), "an alphabet over 256 symbols is refused");
        std::set<std::string> codes;
        bool alphabet_ok = true;
        for (int i = 0; i < 10000; ++i) {
            const std::string c = pairing_random_code(A, 8);
            alphabet_ok = alphabet_ok && code_valid(c);
            codes.insert(c);
        }
        CHECK(alphabet_ok, "10k codes: eight characters from the alphabet");
        CHECK(codes.size() >= 9999, "10k codes are distinct (a 2^40 space: at most a freak collision)");
        // each thread draws from the kernel, not from a per-thread engine seeded once
        std::string a, b;
        std::thread([&] { a = pairing_random_code(A, 32); }).join();
        std::thread([&] { b = pairing_random_code(A, 32); }).join();
        CHECK(a.size() == 32 && b.size() == 32 && a != b, "two threads draw different streams");
        // 31 degrees of freedom: 80 is p ~ 1e-6
        CHECK(chi_square(A, 64000) < 80.0, "32 symbols over 64k draws are uniform");
        // 10 symbols do not divide 256: rejection sampling keeps them even (9 d.o.f.; 40 is p ~ 1e-5)
        CHECK(chi_square("0123456789", 100000) < 40.0, "10 symbols over 100k draws are uniform (no modulo bias)");
        // plain `byte % 10` would give digits 0-5 60.9% of the draws instead of 60.0%: ~1900 extra in 200k
        const std::string d = pairing_random_code("0123456789", 200000);
        long low = 0;
        for (char c : d) low += c < '6';
        CHECK(low > 119000 && low < 121000, "digits 0-5 take 60% of the draws, not 60.9%");
    }

    PairingStore store;

    // --- A: generate -> approve -> revoke (platform tg) ---
    auto code = store.generate_code("tg", "u1", "Alice");
    CHECK(code.has_value(), "generate_code returns a code");
    CHECK(code_valid(*code), "code is 8 chars from alphabet");
    CHECK(!store.is_approved("tg", "u1"), "not approved before");
    auto res = store.approve_code("tg", *code);
    CHECK(res.has_value(), "approve_code succeeds");
    if (res) {
        const Json* uid = json_get(*res, "user_id");
        const Json* nm = json_get(*res, "user_name");
        CHECK(uid && uid->str == "u1", "approved user_id");
        CHECK(nm && nm->str == "Alice", "approved user_name");
    }
    CHECK(store.is_approved("tg", "u1"), "approved after");
    auto appr = store.list_approved("tg");
    CHECK(appr.size() == 1, "list_approved one record");
    if (!appr.empty()) {
        const Json* p = json_get(appr[0], "platform");
        const Json* u = json_get(appr[0], "user_id");
        CHECK(p && p->str == "tg", "record platform");
        CHECK(u && u->str == "u1", "record user_id");
    }
    CHECK(store.revoke("tg", "u1"), "revoke returns true");
    CHECK(!store.is_approved("tg", "u1"), "not approved after revoke");

    // --- B: rate limit (platform dc) ---
    auto c1 = store.generate_code("dc", "x");
    CHECK(c1.has_value(), "first code issued");
    auto c2 = store.generate_code("dc", "x");
    CHECK(!c2.has_value(), "second immediate request rate-limited");

    // --- C: max pending per platform (platform ig) ---
    auto a = store.generate_code("ig", "a");
    auto b = store.generate_code("ig", "b");
    auto d = store.generate_code("ig", "c");
    CHECK(a && b && d, "three pending codes");
    auto over = store.generate_code("ig", "d");
    CHECK(!over.has_value(), "fourth blocked by MAX_PENDING");
    auto pend = store.list_pending("ig");
    CHECK(pend.size() == 3, "list_pending shows 3");
    int cleared = store.clear_pending("ig");
    CHECK(cleared == 3, "clear_pending returns 3");

    // --- D: lockout after 5 failed approvals (platform wx) ---
    for (int i = 0; i < 5; ++i) {
        auto bad = store.approve_code("wx", "BADBAD01");
        CHECK(!bad.has_value(), "bad code rejected");
    }
    CHECK(store.is_locked_out("wx"), "platform locked out after 5 failures");
    auto blocked = store.generate_code("wx", "z");
    CHECK(!blocked.has_value(), "generate blocked while locked out");

    std::filesystem::remove_all(dir, ec);

    if (failures) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "all pairing tests passed\n";
    return 0;
}
