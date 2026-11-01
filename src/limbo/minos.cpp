// src/limbo/minos.cpp — impl of dd/limbo/minos.hpp.
#include "dd/limbo/minos.hpp"

#include "dd/core/park.hpp"

#include <chrono>
#include <algorithm>
#include <utility>

namespace dd {
namespace limbo {

namespace {
// The state edges. A transition is a CAS on the state alone; two threads can never move the
// same dweller from the same state.
bool cas_state(ShadeSlot& s, ShadeState from, ShadeState to) {
    return s.state.compare_exchange_strong(from, to, std::memory_order_acq_rel);
}

std::string alias_of(const Shade* shade) {
    const Props p = shade->props();
    return p.model;
}

// Whether this dweller can answer this turn at all. A turn carrying messages needs a dweller
// that can speak text; a turn carrying ids needs one with a pipe to the engine. A dweller
// waiting for its lexicon is not a fallback: it is a dweller that cannot answer yet.
bool can_answer(const ShadeSlot& s, const TurnRequest& req) {
    if (!req.ids.empty()) return s.shade->spec().kind == ShadeKind::Strata;
    if (!req.messages.empty()) return s.shade->speaks_text();
    return false;   // an empty turn is not a turn
}

// Admitted or answering. ADMITTING is not live: a load is not a turn, and a turn sent to a
// dweller that is still loading is a turn the dweller cannot read.
bool is_live(const ShadeSlot& s) {
    const ShadeState st = s.state.load(std::memory_order_acquire);
    return st == ShadeState::ADMITTED || st == ShadeState::BUSY;
}
}  // namespace

const char* shade_state_name(ShadeState s) {
    switch (s) {
        case ShadeState::DEAD: return "dead";
        case ShadeState::ADMITTING: return "admitting";
        case ShadeState::ADMITTED: return "admitted";
        case ShadeState::BUSY: return "busy";
        case ShadeState::REFUSED: return "refused";
    }
    return "unknown";
}

bool Minos::admit(size_t i, std::string& error) {
    if (i >= slots_.size()) {
        error = "no dweller at index " + std::to_string(i);
        return false;
    }
    if (stopped_.load(std::memory_order_acquire)) {
        error = "the limbo level is stopped; this turn was not sent";
        return false;
    }
    ShadeSlot& s = slots_[i];
    if (!cas_state(s, ShadeState::DEAD, ShadeState::ADMITTING)) {
        const ShadeState was = s.state.load(std::memory_order_acquire);
        if (was == ShadeState::ADMITTED || was == ShadeState::BUSY) return true;   // already in
        if (was == ShadeState::ADMITTING) {
            error = std::string(s.shade->spec().name) + " is still loading; this turn was not sent";
            return false;
        }
        error = std::string(s.shade->spec().name) + " was refused earlier";
        return false;
    }
    if (!s.shade->start(error)) {
        s.state.store(ShadeState::REFUSED, std::memory_order_release);
        if (error.empty()) error = std::string(s.shade->spec().name) + " did not come up";
        return false;
    }
    s.state.store(ShadeState::ADMITTED, std::memory_order_release);
    return true;
}

Route Minos::route(const TurnRequest& req) {
    Route r;
    if (req.ids.empty() && req.messages.empty()) {
        r.reason = "the turn carries neither messages nor ids; nothing was sent";
        return r;
    }
    // An override names a model; the dweller that reports that alias owns the turn. An alias
    // nobody reports is a refusal, not a fallback — a silently substituted model is a lie.
    if (!req.model_override.empty()) {
        bool reported = false;
        for (size_t i = 0; i < slots_.size(); ++i) {
            if (alias_of(slots_[i].shade) != req.model_override) continue;
            reported = true;
            if (!can_answer(slots_[i], req)) {
                r.index = i;
                r.reason = std::string(slots_[i].shade->spec().name) + " reports '" + req.model_override +
                           "' but cannot answer this turn: it has no lexicon for text";
                return r;
            }
            if (slots_[i].state.load(std::memory_order_acquire) == ShadeState::ADMITTING) {
                r.index = i;
                r.reason = std::string(slots_[i].shade->spec().name) + " is still loading";
                return r;
            }
            if (!is_live(slots_[i])) {
                r.index = i;
                r.reason = std::string(slots_[i].shade->spec().name) + " is not admitted; this turn was not sent";
                return r;
            }
            r.index = i;
            r.kind = slots_[i].shade->spec().kind;
            r.reason = "model override '" + req.model_override + "' is reported by " + std::string(slots_[i].shade->spec().name);
            r.ok = true;
            return r;
        }
        r.reason = "model '" + req.model_override + "' is not admitted here" +
                   (reported ? "" : " (no dweller reports that alias)");
        return r;
    }
    if (req.kind != ShadeKind::None) {
        for (size_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].shade->spec().kind != req.kind) continue;
            if (!can_answer(slots_[i], req)) {
                r.index = i;
                r.reason = std::string(slots_[i].shade->spec().name) + " cannot answer this turn: it has no lexicon for text";
                return r;
            }
            if (!is_live(slots_[i])) {
                r.index = i;
                r.reason = std::string(slots_[i].shade->spec().name) + " is not admitted; this turn was not sent";
                return r;
            }
            r.index = i;
            r.kind = req.kind;
            r.reason = std::string("a turn asking for ") + shade_kind_name(req.kind);
            r.ok = true;
            return r;
        }
        r.reason = std::string("no ") + shade_kind_name(req.kind) + " dweller in this level";
        return r;
    }
    // No preference: the first admitted dweller that can answer what it was given. Order is the
    // order the level was built in — the packed MoE answers, llama is the fallback. A dweller
    // that cannot speak a text turn is skipped, and the reason says why, so a level with no
    // lexicon wired says "no dweller can answer a text turn yet", not "no dweller is admitted".
    bool any_admitted = false;
    for (size_t i = 0; i < slots_.size(); ++i) {
        if (!is_live(slots_[i])) continue;
        any_admitted = true;
        if (!can_answer(slots_[i], req)) continue;
        r.index = i;
        r.kind = slots_[i].shade->spec().kind;
        r.reason = std::string(slots_[i].shade->spec().name) + " is the admitted dweller";
        r.ok = true;
        return r;
    }
    r.reason = any_admitted ? "no admitted dweller can answer this turn (text needs the lexicon)"
                            : "no dweller is admitted; nothing was sent";
    return r;
}

bool Minos::dispatch(const Route& r, const TurnRequest& req, const DeltaSink& sink, TurnResult& out, std::string& error) {
    if (!r.ok || r.index >= slots_.size()) {
        error = r.reason.empty() ? "no dweller for this turn" : r.reason;
        return false;
    }
    ShadeSlot& s = slots_[r.index];
    if (!admit(r.index, error)) return false;

    const int ceiling = s.shade->props().slots > 0 ? s.shade->props().slots : 1;
    int had = s.busy.load(std::memory_order_acquire);
    while (had < ceiling) {
        if (s.busy.compare_exchange_weak(had, had + 1, std::memory_order_acq_rel)) break;
    }
    if (had >= ceiling) {
        error = std::string(s.shade->spec().name) + " has all " + std::to_string(ceiling) +
                " slots in flight; this turn was not sent";
        return false;
    }
    s.state.store(ShadeState::BUSY, std::memory_order_release);

    const auto began = std::chrono::steady_clock::now();
    const bool ran = s.shade->run(req, sink, out, error);
    const long took = static_cast<long>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - began).count());

    // A stop that landed while this turn was running is the owner's decision, so the turn's end
    // must not re-admit the dweller nor re-count a slot the stop already cleared.
    const bool stopped_now = stopped_.load(std::memory_order_acquire);
    const int left = stopped_now ? 0 : s.busy.fetch_sub(1, std::memory_order_acq_rel) - 1;
    s.state.store(stopped_now ? ShadeState::DEAD : (left > 0 ? ShadeState::BUSY : ShadeState::ADMITTED),
                  std::memory_order_release);
    s.busy_us.fetch_add(took, std::memory_order_relaxed);
    s.tokens.fetch_add(out.generated, std::memory_order_relaxed);
    s.reused.fetch_add(out.reused, std::memory_order_relaxed);
    if (ran) {
        s.turns.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    s.failed.fetch_add(1, std::memory_order_relaxed);
    if (!s.shade->alive()) {
        // The dweller is gone, not merely unhappy: mark it DEAD, then bring it back if the
        // budget still allows. A turn that failed is reported as failed either way.
        s.state.store(ShadeState::DEAD, std::memory_order_release);
        std::string revived;
        if (!stopped_now && respawn(r.index, revived)) s.state.store(ShadeState::ADMITTED, std::memory_order_release);
        else if (!revived.empty() && error.empty()) error = revived;
    }
    if (error.empty()) error = std::string(s.shade->spec().name) + " gave no reason for this failure";
    return false;
}

bool Minos::respawn(size_t i, std::string& error) {
    if (i >= slots_.size()) return false;
    // A stopped level never respawns: the backoff below would park a thread that is trying to
    // tear the process down, and the budget would be spent on a restart nobody asked for.
    if (stopped_.load(std::memory_order_acquire)) {
        error = "the limbo level is stopped; this dweller will not come back";
        return false;
    }
    ShadeSlot& s = slots_[i];
    if (respawns_total_.fetch_add(1, std::memory_order_relaxed) >= respawn_budget_) {
        s.state.store(ShadeState::REFUSED, std::memory_order_release);
        error = std::string(s.shade->spec().name) + " respawn budget spent; this level will not try again";
        return false;
    }
    s.respawns.fetch_add(1, std::memory_order_relaxed);
    s.state.store(ShadeState::DEAD, std::memory_order_release);
    s.shade->stop();
    // A crash loop is given a ramp, not a tight retry: a dweller that dies on load gets
    // slower and slower attempts, so the level cannot spin on it.
    const long backoff_ms = 250 * (1LL << std::min<long>(s.respawns.load(std::memory_order_relaxed) - 1, 4));
    std::atomic<uint32_t> idle{0};
    core::park(idle, 0, std::chrono::milliseconds(backoff_ms));
    return admit(i, error);
}

void Minos::stop_all() {
    // Mark the decision before touching any process: a turn completing while this runs must see
    // `stopped`, not the state it left behind. Otherwise a stop mid-flight re-admits a dweller
    // whose process is already gone.
    stopped_.store(true, std::memory_order_release);
    for (auto& s : slots_) {
        s.shade->stop();
        s.state.store(ShadeState::DEAD, std::memory_order_release);
        s.busy.store(0, std::memory_order_release);
    }
}

std::string Minos::metrics() const {
    std::string out;
    out += "# HELP limbo_shade_state 0 dead, 1 admitting, 2 admitted, 3 busy, 4 refused\n";
    out += "# TYPE limbo_shade_state gauge\n";
    out += "# HELP limbo_shade_turns_total turns answered by a dweller\n";
    out += "# TYPE limbo_shade_turns_total counter\n";
    out += "# HELP limbo_shade_busy_us microseconds a dweller has spent inside a turn\n";
    out += "# TYPE limbo_shade_busy_us counter\n";
    for (size_t i = 0; i < slots_.size(); ++i) {
        const ShadeSlot& s = slots_[i];
        const std::string name = s.shade->spec().name;
        out += "limbo_shade_state{name=\"" + name + "\"} " + std::to_string(static_cast<int>(s.state.load(std::memory_order_acquire))) + "\n";
        out += "limbo_shade_turns_total{name=\"" + name + "\"} " + std::to_string(s.turns.load(std::memory_order_relaxed)) + "\n";
        out += "limbo_shade_tokens_total{name=\"" + name + "\"} " + std::to_string(s.tokens.load(std::memory_order_relaxed)) + "\n";
        out += "limbo_shade_reused_tokens_total{name=\"" + name + "\"} " + std::to_string(s.reused.load(std::memory_order_relaxed)) + "\n";
        out += "limbo_shade_failed_total{name=\"" + name + "\"} " + std::to_string(s.failed.load(std::memory_order_relaxed)) + "\n";
        out += "limbo_shade_respawns_total{name=\"" + name + "\"} " + std::to_string(s.respawns.load(std::memory_order_relaxed)) + "\n";
        out += "limbo_shade_busy_us{name=\"" + name + "\"} " + std::to_string(s.busy_us.load(std::memory_order_relaxed)) + "\n";
        out += "limbo_shade_inflight{name=\"" + name + "\"} " + std::to_string(s.busy.load(std::memory_order_acquire)) + "\n";
    }
    return out;
}

core::Json Minos::props_json() const {
    core::Json p = core::json_obj();
    core::Json shades = core::json_arr();
    int n_ctx = 0;
    std::string model;
    for (size_t i = 0; i < slots_.size(); ++i) {
        const Props q = slots_[i].shade->props();
        core::Json one = core::json_obj();
        core::obj_set(one, "name", core::json_str(slots_[i].shade->spec().name));
        core::obj_set(one, "kind", core::json_str(shade_kind_name(q.kind)));
        core::obj_set(one, "state", core::json_str(shade_state_name(slots_[i].state.load(std::memory_order_acquire))));
        core::obj_set(one, "model", core::json_str(q.model));
        core::obj_set(one, "n_ctx", core::json_num(q.n_ctx));
        core::obj_set(one, "slots", core::json_num(q.slots));
        core::obj_set(one, "alive", core::json_bool(q.alive));
        for (const auto& kv : q.facts.obj) core::obj_set(one, kv.first, *kv.second);
        core::arr_push(shades, one);
        if (q.n_ctx > n_ctx) n_ctx = q.n_ctx;
        if (model.empty() && !q.model.empty()) model = q.model;
    }
    core::obj_set(p, "model", core::json_str(model));
    core::obj_set(p, "n_ctx", core::json_num(n_ctx));
    core::obj_set(p, "dwellers", shades);
    core::obj_set(p, "engine", core::json_str("limbo-level"));
    return p;
}

core::Json Minos::slots_json() const {
    core::Json p = core::json_obj();
    core::Json slots = core::json_arr();
    for (size_t i = 0; i < slots_.size(); ++i) {
        const ShadeSlot& s = slots_[i];
        core::Json one = core::json_obj();
        core::obj_set(one, "index", core::json_num(static_cast<long>(i)));
        core::obj_set(one, "name", core::json_str(s.shade->spec().name));
        core::obj_set(one, "kind", core::json_str(shade_kind_name(s.shade->spec().kind)));
        core::obj_set(one, "state", core::json_str(shade_state_name(s.state.load(std::memory_order_acquire))));
        core::obj_set(one, "inflight", core::json_num(s.busy.load(std::memory_order_acquire)));
        core::obj_set(one, "turns", core::json_num(s.turns.load(std::memory_order_relaxed)));
        core::obj_set(one, "tokens", core::json_num(s.tokens.load(std::memory_order_relaxed)));
        core::obj_set(one, "reused", core::json_num(s.reused.load(std::memory_order_relaxed)));
        core::obj_set(one, "failed", core::json_num(s.failed.load(std::memory_order_relaxed)));
        core::obj_set(one, "respawns", core::json_num(s.respawns.load(std::memory_order_relaxed)));
        core::arr_push(slots, one);
    }
    core::obj_set(p, "slots", slots);
    return p;
}

}  // namespace limbo
}  // namespace dd
