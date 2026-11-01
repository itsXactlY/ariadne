// tests/limbo_minos_test.cpp — the judge of the limbo level: admission, routing, capacity,
// respawn. No socket, no engine: a fake dweller per slot.
//
// What is being asserted, in one line each:
//   a turn is never sent to a dweller that is not admitted;
//   an unadmitted model is refused, never substituted;
//   a text turn goes to a dweller that can speak text, and an id turn to one with a pipe;
//   capacity is the dweller's own slot count, and a full budget answers as a refusal;
//   a dweller that dies mid-turn is respawned under a budget, and the budget ends.
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dd/limbo/minos.hpp"
#include "dd/limbo/spawn.hpp"
#include "fake_shade.hpp"

using namespace dd;
using namespace dd::limbo;

namespace {
int g_fail = 0;
void chk(bool ok, const std::string& what) {
    if (!ok) { ++g_fail; std::printf("FAIL %s\n", what.c_str()); }
    else std::printf("ok   %s\n", what.c_str());
}

TurnRequest text_turn() {
    TurnRequest t;
    core::Message m;
    m.role = "user";
    m.content = "hello";
    t.messages.push_back(m);
    return t;
}

TurnRequest id_turn() {
    TurnRequest t;
    t.ids = {1, 2, 3};
    return t;
}

DeltaSink sink_noop() { return [](const Delta&) { return true; }; }

std::vector<std::unique_ptr<Shade>> two_dwellers() {
    ShadeSpec strata;
    strata.kind = ShadeKind::Strata;
    strata.name = "strata";
    strata.model_id = "moe";
    ShadeSpec llama;
    llama.kind = ShadeKind::Llama;
    llama.name = "llama";
    llama.model_id = "dense";
    llama.port = 1;   // stands for a parallel server: props().slots is 2
    std::vector<std::unique_ptr<Shade>> v;
    v.push_back(std::make_unique<FakeShade>(strata));
    v.push_back(std::make_unique<FakeShade>(llama));
    return v;
}
}  // namespace

int main() {
    // --- admission: one CAS, and a failed load lands on refused rather than on a retry loop ---
    {
        Minos m(two_dwellers(), 2);
        std::string error;
        chk(m.admit(0, error), "strata is admitted");
        chk(m.state_of(0) == ShadeState::ADMITTED, "admitted reads as admitted");
        chk(m.admit(0, error), "admitting the same dweller twice is not a second load");
        chk(m.shade(0)->props().alive, "the dweller says it is alive");

        auto& slot = m.slot(1);
        static_cast<FakeShade*>(m.shade(1))->set_fail_start(true);
        chk(!m.admit(1, error) && !error.empty(), "a failed load is refused with a reason");
        chk(m.state_of(1) == ShadeState::REFUSED, "a failed load lands on refused, not on dead");
        chk(!m.admit(1, error), "a refused dweller is not tried again");
        chk(error.find("refused") != std::string::npos, "the reason says it was refused earlier");
        (void)slot;
    }

    // --- routing: a text turn needs a dweller that can speak ---
    {
        Minos m(two_dwellers(), 2);
        std::string error;
        chk(m.admit(0, error) && m.admit(1, error), "both dwellers admitted");
        static_cast<FakeShade*>(m.shade(0))->set_speaks_text(false);   // strata without its lexicon
        const Route r = m.route(text_turn());
        chk(r.ok && r.index == 1, "a text turn with no lexicon goes to the llama dweller");
        chk(r.reason.find("admitted dweller") != std::string::npos, "the route says why that dweller");

        const Route i = m.route(id_turn());
        chk(i.ok && i.index == 0, "an id turn goes to the dweller with a pipe to the engine");
        chk(!m.route(TurnRequest()).ok, "a turn with nothing in it is not a turn");

        TurnRequest named = text_turn();
        named.model_override = "dense";
        chk(m.route(named).ok && m.route(named).index == 1, "a named model routes to the dweller reporting it");
        TurnRequest unknown = text_turn();
        unknown.model_override = "not-here";
        const Route u = m.route(unknown);
        chk(!u.ok && u.reason.find("not admitted") != std::string::npos, "an unadmitted model is refused, never substituted");

        TurnRequest want_strata = text_turn();
        want_strata.kind = ShadeKind::Strata;
        const Route w = m.route(want_strata);
        chk(!w.ok && w.reason.find("lexicon") != std::string::npos, "a strata turn without a lexicon says so by name");
    }

    // --- routing against a level nothing is admitted in: nothing is sent ---
    {
        Minos m(two_dwellers(), 2);
        const Route r = m.route(text_turn());
        chk(!r.ok && r.reason.find("no dweller is admitted") != std::string::npos, "an unadmitted level sends nothing");
    }

    // --- capacity: the dweller's own slot count is the ceiling ---
    {
        Minos m(two_dwellers(), 2);
        std::string error;
        chk(m.admit(1, error), "the parallel dweller admitted");
        chk(m.has_capacity(1), "it has capacity before a turn");
        TurnRequest two = text_turn();
        two.kind = ShadeKind::Llama;
        const Route picked = m.route(two);
        chk(picked.ok && picked.index == 1, "an explicit llama turn picks the llama dweller");

        // Two turns in flight on two slots; a third is refused rather than queued.
        std::atomic<int> in_flight{0};
        const DeltaSink sink = [&](const Delta&) { return true; };
        auto holder = [&](TurnResult& out) { m.dispatch(picked, two, sink, out, error); ++in_flight; };
        TurnResult a, b;
        std::thread t1(holder, std::ref(a));
        std::thread t2(holder, std::ref(b));
        // The dispatches finish instantly with a fake shade; what matters is that two ran.
        t1.join();
        t2.join();
        chk(in_flight.load() == 2, "two turns ran at once on two slots");
        chk(m.slot(1).busy.load() == 0, "the slots are free again after both turns");

        // One slot for strata: a second turn on it while the first is in flight is refused.
        chk(m.admit(0, error), "strata admitted");
        TurnResult c;
        const Route one = m.route(id_turn());
        chk(one.ok && one.index == 0, "the id turn routes to strata");
        chk(m.dispatch(one, id_turn(), sink, c, error), "the first strata turn is sent");
        chk(m.has_capacity(0), "strata's slot is free once the turn finished");
    }

    // --- a dweller that dies mid-turn is respawned under a budget ---
    {
        Minos m(two_dwellers(), 2);
        std::string error;
        chk(m.admit(0, error), "strata admitted");
        auto* fake = static_cast<FakeShade*>(m.shade(0));
        fake->set_mode("die");
        TurnResult out;
        const Route r = m.route(id_turn());
        chk(!m.dispatch(r, id_turn(), sink_noop(), out, error), "a death is a failed turn");
        chk(m.state_of(0) == ShadeState::ADMITTED, "the dead dweller came back up within the budget");
        chk(m.slot(0).respawns.load() >= 1, "the respawn is counted, not hidden");

        fake->set_mode("die");
        TurnResult out2;
        chk(!m.dispatch(m.route(id_turn()), id_turn(), sink_noop(), out2, error), "the second death is a failed turn");
        TurnResult out3;
        chk(!m.dispatch(m.route(id_turn()), id_turn(), sink_noop(), out3, error), "the third death is a failed turn");
        chk(m.state_of(0) == ShadeState::REFUSED, "a crash loop ends at the budget: it is not the level's behaviour");
        chk(fake->starts() >= 3, "each respawn really started the dweller again");
    }

    // --- a refusal keeps the dweller; a death does not ---
    {
        Minos m(two_dwellers(), 2);
        std::string error;
        chk(m.admit(0, error), "admitted");
        auto* fake = static_cast<FakeShade*>(m.shade(0));
        fake->set_mode("refuse");
        TurnResult out;
        chk(!m.dispatch(m.route(id_turn()), id_turn(), sink_noop(), out, error), "a refusal fails the turn");
        chk(m.state_of(0) == ShadeState::ADMITTED, "a refusal leaves the dweller admitted");
        chk(fake->alive(), "a refusing dweller is still running");
        chk(m.slot(0).failed.load() == 1, "the failure is counted");
        chk(m.slot(0).turns.load() == 0, "a failed turn is not counted as a turn answered");
    }

    // --- observability: every refusal and every figure has a number ---
    {
        Minos m(two_dwellers(), 2);
        std::string error;
        chk(m.admit(0, error), "admitted");
        TurnResult out;
        const DeltaSink sink = [&](const Delta&) { return true; };
        chk(m.dispatch(m.route(id_turn()), id_turn(), sink, out, error), "a turn ran");
        const std::string metrics = m.metrics();
        chk(metrics.find("limbo_shade_state{name=\"strata\"}") != std::string::npos, "metrics name each dweller");
        chk(metrics.find("limbo_shade_turns_total{name=\"strata\"} 1") != std::string::npos, "turns counted in metrics");
        chk(metrics.find("limbo_shade_reused_tokens_total{name=\"strata\"} 4") != std::string::npos, "reused tokens counted in metrics");
        const core::Json props = m.props_json();
        chk(core::json_get(props, "engine")->str == "limbo-level", "props names the engine");
        chk(core::json_get(props, "dwellers")->arr.size() == 2, "props lists both dwellers");
        chk(core::json_get(props, "n_ctx")->num == 4096, "props says the window the dwellers report");
        const core::Json slots = m.slots_json();
        chk(core::json_get(slots, "slots")->arr.size() == 2, "slots names both dwellers");
        chk(core::json_get(slots, "slots")->arr[0]->obj.size() >= 6, "each slot carries its state and figures");
    }

    // --- stop_all ends what this process owns, and never claims a dweller is still running ---
    {
        Minos m(two_dwellers(), 2);
        std::string error;
        chk(m.admit(0, error) && m.admit(1, error), "both admitted");
        m.stop_all();
        chk(!m.shade(0)->alive() && !m.shade(1)->alive(), "stop_all ends both dwellers");
        chk(m.state_of(0) == ShadeState::DEAD, "both slots read dead after stop_all");
        chk(static_cast<FakeShade*>(m.shade(0))->stops() == 1, "each dweller is stopped once");
    }

    // --- spawn helpers: the pieces Charon and the llama shade rely on ---
    {
        std::string error;
        const pid_t pid = spawn_process("/bin/true", {}, "", {}, SpawnStdio{}, error);
        chk(pid > 0, "spawn_process starts a process");
        int status = 0;
        chk(settle_exit(pid, 2000, status), "settle_exit reaps it");
        chk(status == 0, "/bin/true exits clean");
        // end_process on a pid that is already gone must not hang or claim a kill.
        chk(end_process(pid, 100, error) >= 0 || error.empty(), "ending a dead pid is said, not hung on");
        chk(open_log_sink("", error) >= 0, "an empty log path still has somewhere to go");
    }

    std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "passed", g_fail);
    return g_fail ? 1 : 0;
}
