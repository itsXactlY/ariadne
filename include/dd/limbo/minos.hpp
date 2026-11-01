#pragma once
// dd/limbo/minos.hpp — the judge of the limbo level: who is admitted, who is busy, who gets
// the next turn.
//
// Minos decides, he does not generate. Everything in here is an atomic transition or a CAS;
// there is no mutex and no lock ordering to reason about, because the only contested thing is
// a small integer per dweller:
//   DEAD ──admit──> ADMITTING ──start ok──> ADMITTED ──turn──> BUSY ──done──> ADMITTED
//                       │                       │
//                       └── start failed ───────┴── refused ──> REFUSED
//
// An unadmitted model is refused. The level never lazily starts a dweller to answer a turn:
// starting is one operation, with one owner, and a turn that silently spawns a 23 GB load is
// a turn nobody asked for.
//
// A dweller whose pipe dies mid-turn is marked DEAD and respawned under a budget — a crash
// loop is not allowed to become the level's behaviour.

#include <atomic>
#include <cstddef>
#include <memory>
#include <deque>
#include <string>
#include <vector>

#include "dd/limbo/shade.hpp"

namespace dd {
namespace limbo {

enum class ShadeState : int {
    DEAD = 0,       // not running; may be admitted
    ADMITTING = 1,  // loading; turns are refused, not queued
    ADMITTED = 2,   // answers
    BUSY = 3,       // answers, but one turn is already in flight
    REFUSED = 4,    // gave up: this process will not try again
};

const char* shade_state_name(ShadeState s);

struct ShadeSlot {
    Shade* shade = nullptr;
    std::atomic<ShadeState> state{ShadeState::DEAD};
    std::atomic<int> busy{0};      // turns in flight; the dweller's `slots` is the ceiling
    std::atomic<long> turns{0};
    std::atomic<long> tokens{0};
    std::atomic<long> reused{0};   // window tokens not re-prefilled
    std::atomic<long> failed{0};
    std::atomic<long> respawns{0};
    std::atomic<long> busy_us{0};
};

struct Route {
    size_t index = 0;
    ShadeKind kind = ShadeKind::Strata;
    std::string reason;            // why this dweller, in one line — /slots shows it
    bool ok = false;
};

class Minos {
public:
    explicit Minos(std::vector<std::unique_ptr<Shade>> shades, long respawn_budget = 5)
        : respawn_budget_(respawn_budget) {
        for (size_t i = 0; i < shades.size(); ++i) slots_.emplace_back();
        for (size_t i = 0; i < shades.size(); ++i) slots_[i].shade = shades[i].get();
        owned_ = std::move(shades);
    }

    size_t count() const { return slots_.size(); }
    ShadeSlot& slot(size_t i) { return slots_[i]; }
    const ShadeSlot& slot(size_t i) const { return slots_[i]; }
    Shade* shade(size_t i) { return i < owned_.size() ? owned_[i].get() : nullptr; }

    // Admit one dweller. Returns false with the reason; a failed load lands on REFUSED so the
    // level reports it instead of trying forever.
    bool admit(size_t i, std::string& error);

    // Which dweller answers this turn. `kind == ShadeKind::None` means "whatever is admitted".
    Route route(const TurnRequest& req);

    // Run a turn on the dweller `route` picked. Takes a slot, or returns false without sending
    // anything. A pipe that died mid-turn marks the dweller DEAD and respawns it if the budget
    // still has room.
    bool dispatch(const Route& r, const TurnRequest& req, const DeltaSink& sink, TurnResult& out, std::string& error);

    // Respawn a dweller that died. false when the budget is spent or it is already coming up.
    bool respawn(size_t i, std::string& error);

    void refuse(size_t i, const std::string& why) {
        (void)why;
        slots_[i].state.store(ShadeState::REFUSED, std::memory_order_release);
        slots_[i].shade->stop();
    }

    // What a dweller is, right now, for the front's status code: ADMITTING is a 409, a full
    // slot budget is a 429, a dead one is a 502. The level decides these from state, not from
    // matching on error text.
    ShadeState state_of(size_t i) const { return i < slots_.size() ? slots_[i].state.load(std::memory_order_acquire) : ShadeState::DEAD; }
    bool has_capacity(size_t i) const {
        if (i >= slots_.size()) return false;
        const int ceiling = slots_[i].shade->props().slots > 0 ? slots_[i].shade->props().slots : 1;
        return slots_[i].busy.load(std::memory_order_acquire) < ceiling;
    }

    // How many dwellers answer right now. /health reports it, and a test asks for it instead of
    // reading five states by hand.
    size_t admitted() const {
        size_t n = 0;
        for (const auto& s : slots_) {
            const ShadeState st = s.state.load(std::memory_order_acquire);
            if (st == ShadeState::ADMITTED || st == ShadeState::BUSY) ++n;
        }
        return n;
    }

    void stop_all();

    // True once this judge has been told to stop. A turn still in flight must not put a dweller
    // back to ADMITTED after the level was stopped: stop is an owner's decision, not a state the
    // last turn happens to leave behind.
    bool stopped() const { return stopped_.load(std::memory_order_acquire); }

    // One Prometheus-shaped block: one line per dweller, each with its state and its counters.
    std::string metrics() const;

    // The aggregate /props: what this level is, as one JSON object.
    core::Json props_json() const;

    // The /slots view: state, in-flight turns, and what each dweller reports.
    core::Json slots_json() const;

private:
    // A deque, not a vector: ShadeSlot holds atomics, which are neither movable nor copyable,
    // so a growing vector cannot relocate them. A deque keeps every reference stable forever.
    std::vector<std::unique_ptr<Shade>> owned_;
    std::deque<ShadeSlot> slots_;
    long respawn_budget_ = 5;
    std::atomic<long> respawns_total_{0};
    std::atomic<bool> stopped_{false};
};

}  // namespace limbo
}  // namespace dd
