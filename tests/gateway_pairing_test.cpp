// Verifies dd::gateway::PairingStore against gateway/pairing.py.
#include "dd/gateway/pairing.hpp"
#include "dd/core/home.hpp"
#include "dd/core/json.hpp"

#include <filesystem>
#include <iostream>
#include <string>

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

int main() {
    set_daedalus_home_override("/tmp/dd_pairing_test");
    std::error_code ec;
    std::filesystem::remove_all("/tmp/dd_pairing_test", ec);

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

    std::filesystem::remove_all("/tmp/dd_pairing_test", ec);

    if (failures) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "all pairing tests passed\n";
    return 0;
}
