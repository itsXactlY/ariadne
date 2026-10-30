// dd::learn::LearnService — events in, outcomes and a skill out, state back after a restart.
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ctime>
#include <sstream>

#include "learn_tmp.hpp"
#include "dd/learn/service.hpp"

namespace fs = std::filesystem;
using namespace dd;
using namespace dd::learn;

static int g_fail = 0;
static void chk(bool ok, const std::string& label) {
    if (!ok) { ++g_fail; std::printf("FAIL %s\n", label.c_str()); }
}

static know::Event ev(know::Event::Type t, uint32_t turn, uint32_t round) {
    know::Event e;
    e.type = t;
    e.turn = turn;
    e.round = round;
    e.ts_ns = 1000000000ull * (1700000000ull + turn * 100 + round);
    return e;
}

static know::Event call(uint32_t turn, uint32_t round, bool failed, const std::string& err) {
    know::Event e = ev(know::Event::Type::Calls, turn, round);
    e.call.tool = "run_terminal";
    e.call.args = "{\"command\":\"systemctl --user restart mazemaker.pod\"}";
    e.call.digest = "run_terminal|" + std::to_string(round) + (failed ? "f" : "ok");
    e.call.binaries = {"systemctl"};
    e.call.failed = failed;
    e.call.exit = failed ? 1 : 0;
    e.call.error = err;
    e.key = err;
    return e;
}

static know::Uri skill_uri(const std::string& id) {
    know::Uri u;
    u.kind = know::Kind::Skill;
    u.id = id;
    return u;
}

static ServiceOptions opts(const fs::path& root) {
    ServiceOptions o;
    o.dir = root / "learn";
    o.skills_root = root / "skills";
    o.home_dir = "/home/nobody";
    o.project_dir = root.string();
    o.curate_every = std::chrono::seconds(3600);
    return o;
}

static bool wait_for(const std::function<bool()>& f, int ms = 3000) {
    for (int i = 0; i < ms / 5; ++i) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return f();
}

static size_t skill_files(const fs::path& root) {
    size_t n = 0;
    std::error_code ec;
    if (!fs::exists(root, ec)) return 0;
    for (const auto& p : fs::recursive_directory_iterator(root))
        if (p.path().filename() == "SKILL.md") ++n;
    return n;
}

int main() {
    const fs::path base = learn_test_dir("learn_service_tmp");
    fs::remove_all(base);
    fs::create_directories(base);

    // ---- off switch ----
    {
        fs::create_directories(base / "off" / "learn");
        std::ofstream(base / "off" / "learn" / "OFF") << "x";
        std::string err;
        chk(!LearnService::start(opts(base / "off"), &err) && !err.empty(), "an OFF file keeps the service from starting");
    }

    const fs::path root = base / "run";
    const know::Uri qs = skill_uri("podman-quadlet-ops");
    {
        std::string err;
        auto svc = LearnService::start(opts(root), &err);
        chk(svc != nullptr, "the service starts: " + err);
        if (!svc) return 1;
        auto sink = svc->sink();

        // turn 1: an offer the model opens, then a failure that stops coming back -> a skill
        sink->emit(ev(know::Event::Type::TurnStart, 1, 0));
        know::Event off = ev(know::Event::Type::Offered, 1, 0);
        off.uri = qs;
        off.reason = "turn";
        off.matched = 3;
        off.mass = 9;
        off.score = 0.4f;
        off.terms = {"podman", "quadlet", "restart"};
        sink->emit(std::move(off));
        know::Event op = ev(know::Event::Type::OpenedByModel, 1, 1);
        op.uri = qs;
        sink->emit(std::move(op));
        const std::string err1 = "Failed to restart mazemaker.pod: Unit mazemaker.pod not found";
        sink->emit(call(1, 1, true, err1));
        sink->emit(call(1, 2, true, err1));
        sink->emit(call(1, 3, false, ""));
        sink->emit(call(1, 4, false, ""));
        sink->emit(call(1, 5, false, ""));
        sink->emit(call(1, 6, false, ""));
        sink->emit(ev(know::Event::Type::TurnEnd, 1, 6));

        chk(svc->wait_idle(std::chrono::seconds(3)), "every event is processed");
        svc->curate_now();
        chk(wait_for([&] { return svc->status().passes >= 2; }), "a curator pass ran");
        const auto st = svc->status();
        chk(st.dropped == 0, "nothing was dropped");
        chk(st.outcomes >= 1, "the offer closed into an outcome");
        chk(st.drafts == 1 && skill_files(root / "skills") == 1, "a solved failure became one skill on disk (drafts " + std::to_string(st.drafts) + ", rejected " + std::to_string(st.rejected) + ")");
        chk(st.ledger.appended > 0 && st.ledger.write_errors == 0, "the ledger journaled without errors");
        svc->stop();
        chk(svc->status().ledger.appended > 0, "stop flushes");
    }

    // ---- restart: the state comes back from the journal ----
    {
        auto svc = LearnService::start(opts(root));
        chk(svc != nullptr, "the service restarts on the same directory");
        if (svc) {
            const auto st = svc->status();
            chk(st.ledger.recovered > 0, "the journal was replayed (" + std::to_string(st.ledger.recovered) + ")");
            // the same problem again must not make a second skill file
            const std::string err1 = "Failed to restart mazemaker.pod: Unit mazemaker.pod not found";
            auto sink = svc->sink();
            sink->emit(ev(know::Event::Type::TurnStart, 2, 0));
            sink->emit(call(2, 1, true, err1));
            sink->emit(call(2, 2, true, err1));
            for (uint32_t r = 3; r <= 6; ++r) sink->emit(call(2, r, false, ""));
            sink->emit(ev(know::Event::Type::TurnEnd, 2, 6));
            svc->wait_idle(std::chrono::seconds(3));
            svc->stop();
            chk(skill_files(root / "skills") == 1, "the same problem again updates, never duplicates");
        }
    }

    // ---- feedback off: nothing is folded ----
    {
        ServiceOptions o = opts(base / "nofb");
        o.feedback = false;
        auto svc = LearnService::start(o);
        chk(svc != nullptr, "starts with feedback off");
        if (svc) {
            know::Event off = ev(know::Event::Type::Offered, 1, 0);
            off.uri = qs;
            svc->sink()->emit(std::move(off));
            svc->wait_idle(std::chrono::seconds(2));
            svc->stop();
            chk(svc->status().outcomes == 0, "feedback off: no outcomes");
        }
    }

    // ---- a burst past the queue cap is dropped and counted, never blocking ----
    {
        auto svc = LearnService::start(opts(base / "burst"));
        if (svc) {
            for (int i = 0; i < 200000; ++i) svc->sink()->emit(ev(know::Event::Type::Rebase, 1, 1));
            // The ceiling is a safety net, not a performance assumption: wait_idle returns the
            // moment the queue is empty. A 10s bound was a silent bet on how fast one event is
            // handled, and a sanitizer build handles it 20x slower, so the test failed at 10s
            // while nothing was wrong. The label carries the numbers so a slow drain is visible.
            const auto t0 = std::chrono::steady_clock::now();
            const bool drained = svc->wait_idle(std::chrono::seconds(120));
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            const auto st = svc->status();
            chk(drained && st.processed == st.pushed,
                "the burst drained (pushed " + std::to_string(st.pushed) + " dropped " +
                std::to_string(st.dropped) + " processed " + std::to_string(st.processed) +
                " in " + std::to_string(ms) + "ms)");
            chk(st.dropped > 0, "past the ring cap the overflow is counted, never blocking");
            chk(st.pushed < 200000, "the ring refuses instead of growing");
            svc->stop();
        }
    }

    // ---- governance without drafting: a learned skill is governed even when author is off ----
    {
        const fs::path g = base / "govern";
        fs::create_directories(g / "skills" / "ops" / "flow-hub");
        std::ofstream(g / "skills" / "ops" / "flow-hub" / "SKILL.md")
            << "---\nname: flow-hub\ncategory: ops\norigin: learned\nstate: probation\n"
               "state_since: " << std::time(nullptr) << "\n---\nbody\n";
        ServiceOptions o = opts(g);
        o.author = false;
        std::string err;
        auto svc = LearnService::start(o, &err);
        chk(svc != nullptr, "govern: the service starts with drafting off: " + err);
        if (svc) {
            const know::Uri hub = skill_uri("flow-hub");
            const int64_t wall = std::time(nullptr);   // this run's own clock: decayed counts must still read as eight
            for (int i = 0; i < 8; ++i) {
                know::Event off = ev(know::Event::Type::Offered, 1, i);
                off.ts_ns = 1000000000ull * wall;
                off.uri = hub;
                off.matched = 3;
                off.mass = 9;
                off.score = 0.4f;
                svc->sink()->emit(std::move(off));
            }
            chk(svc->wait_idle(std::chrono::seconds(3)), "govern: every offer was processed");
            svc->curate_now();
            chk(wait_for([&] { return svc->status().transitions >= 1; }),
                "govern: eight offers with none opened ends probation even with drafting off");
            std::ifstream md(g / "skills" / "ops" / "flow-hub" / "SKILL.md");
            std::stringstream body;
            body << md.rdbuf();
            chk(body.str().find("state: dormant") != std::string::npos, "govern: the hub went dormant in place, on demand evidence");
            chk(body.str().find("state: retired") == std::string::npos, "govern: demand alone never retires");
            chk(svc->status().drafts == 0 && skill_files(g / "skills") == 1, "govern: nothing was drafted while drafting is off");
            svc->stop();
        }
    }

    fs::remove_all(base);
    if (g_fail) std::printf("%d FAILED\n", g_fail);
    else std::printf("learn service: all passed\n");
    return g_fail ? 1 : 0;
}
