// tests/devices_race_test.cpp — the device store under two writers at once.
//
// devices.json is read whole, edited, and written whole by `pair`, `revoke` and
// `--gateway-adopt`. With no interprocess lock the last writer wins the file: a phone
// paired by the other writer simply stops existing, and its token is refused on the next
// request with nothing to explain why. One temp name for both writers made it worse — the
// second rename moved the first one's half-written file into place.

#include "dd/gateway/crypto.hpp"
#include "dd/gateway/devices.hpp"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace c = dd::gateway::crypto;
using dd::gateway::Device;
using dd::gateway::Devices;

static int g_fail = 0;
static void chk(bool ok, const std::string& what) {
    if (!ok) { ++g_fail; std::printf("FAIL %s\n", what.c_str()); }
}

int main() {
    const fs::path dir = fs::temp_directory_path() / ("ariadne-devices-race-" + c::hex(c::random_bytes(6)));
    fs::create_directories(dir);
    Devices dev(dir / "devices.json");

    // Eight pairings at once: every one must still be in the file afterwards.
    std::atomic<int> added{0};
    std::vector<std::thread> writers;
    for (int i = 0; i < 8; ++i) {
        writers.emplace_back([&, i] {
            std::string token, error;
            const auto d = dev.add("phone-" + std::to_string(i), {"chat"}, token, error);
            if (d) added.fetch_add(1);
            else std::printf("add %d failed: %s\n", i, error.c_str());
        });
    }
    for (auto& t : writers) t.join();
    chk(added.load() == 8, "every concurrent add succeeded");
    chk(dev.list().size() == 8, "no row was overwritten by a later writer");

    // A revoke must not erase a row another writer added between its read and its write.
    const auto before = dev.list();
    chk(before.size() == 8, "the eight rows are readable");
    std::atomic<int> revoked{0};
    std::vector<std::thread> mixed;
    mixed.emplace_back([&] {
        std::string error;
        if (dev.revoke(before[0].id, error)) ++revoked;
        else std::printf("revoke failed: %s\n", error.c_str());
    });
    mixed.emplace_back([&] {
        std::string token, error;
        if (dev.add("late", {"chat"}, token, error)) ++revoked;
        else std::printf("late add failed: %s\n", error.c_str());
    });
    for (auto& t : mixed) t.join();
    chk(revoked.load() == 2, "both the revoke and the late add took effect");
    // 8 rows, minus the revoked one, plus the late one. The count the writer promised is
    // the count the file must hold; an eviction of the late row would read 7.
    chk(dev.list().size() == 8, "the late row survived the revoke, and the revoked row is gone");
    chk(!dev.by_id(before[0].id), "the revoked device is refused");
    chk(dev.by_id(before[1].id).has_value(), "an untouched device is still paired");

    // Revoking must remove exactly the one row. Erasing from the hit to end() reads as
    // "at most one row per id" and looks harmless, but it deletes every device after the
    // match: revoking the first row wiped the whole store and every paired phone was
    // refused with no explanation.
    {
        std::string token, error;
        const size_t n = dev.list().size();
        const auto d = dev.add("first-row-marker", {"chat"}, token, error);
        chk(d.has_value(), "a row for the first position was added");
        chk(dev.revoke(before[1].id, error), "revoking a middle row succeeds");
        chk(dev.list().size() == n, "only the revoked row is gone: " + std::to_string(dev.list().size()) + " of " + std::to_string(n));
        chk(!dev.by_id(before[1].id), "the revoked row is refused");
        chk(dev.by_id(d->id).has_value(), "the row after the revoked one survived");
        // An unknown id must fail and leave the file exactly as it was.
        const size_t still = dev.list().size();
        chk(!dev.revoke("dev-nope0000", error), "an unknown id is refused");
        chk(error.find("no device") != std::string::npos, "the error names the id: " + error);
        chk(dev.list().size() == still, "a missed revoke writes nothing");
    }
    fs::remove_all(dir);
    std::printf("%s: device store under concurrent writers (%d failures)\n", g_fail ? "FAILED" : "ok", g_fail);
    return g_fail ? 1 : 0;

}
