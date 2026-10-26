// src/know/router.cpp — impl of dd/know/router.hpp.
#include "dd/know/router.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>

#include "dd/core/json.hpp"
#include "dd/core/park.hpp"
#include "dd/know/text.hpp"

namespace dd {
namespace know {

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

std::string join(const std::vector<std::string>& v, const char* sep) {
    std::string s;
    for (const auto& x : v)
        if (!x.empty()) s += (s.empty() ? "" : sep) + x;
    return s;
}

}  // namespace

Router::Router(Catalogues cats, Policy policy, std::shared_ptr<context::Memory> pod)
    : cats_(std::move(cats)), policy_(std::move(policy)), pod_(pod), recaller_(pod), triggers_(default_triggers()) {}

Catalogue* Router::catalogue(Kind k) const {
    switch (k) {
        case Kind::Skill: return cats_.skills.get();
        case Kind::Memory: return cats_.memory.get();
        case Kind::Tool: return cats_.tools.get();
        default: return nullptr;
    }
}

IdfFn Router::idf_fn() const {
    return [this](const std::string& t) {
        float best = 0;
        for (Kind k : {Kind::Skill, Kind::Memory, Kind::Tool})
            if (const Catalogue* c = catalogue(k)) best = std::max(best, c->idf(t));
        return best;
    };
}

bool Router::offers(const Hit& h) const {
    const Catalogue* c = catalogue(h.card.uri.kind);
    return (h.card.uri.kind == Kind::Memory ? policy_.memory : policy_.skills).offers(h, c ? c->size() : 0);
}

// The demand guard. Two sources of the same verdict: the fold's decayed conversion, which survives a restart,
// and this process's own offers since the card's last open, which acts the moment the eighth refusal lands.
// A refused card is not silenced — it is offered once every probe_turns turns, so it can still earn its way back.
bool Router::refused(const Uri& u) {
    const auto& g = policy_.refusal;
    if (!g.on) return false;
    Refusal& r = refusal_[u.str()];
    const Demand d = overlay_ ? overlay_->demand(u) : Demand{};
    const bool by_fold = d.offered >= g.refuse_offers && d.lcb < g.refuse_floor;
    const bool by_local = r.offers >= g.refuse_offers && r.opens == 0;
    if (!by_fold && !by_local) {
        r.closed_until = 0;   // demand came back: the window is open again
        return false;
    }
    const int t = ws_.turn();
    if (r.closed_until > t) { ++stats_.refused; ++declined_; return true; }
    r.closed_until = t + g.probe_turns;   // the probe: this turn may say it once, then the window closes
    return false;
}

void Router::note_offer(const Uri& u) { ++refusal_[u.str()].offers; }

void Router::note_demand_open(const Uri& u) {
    Refusal& r = refusal_[u.str()];
    ++r.opens;
    r.offers = 0;
    r.closed_until = 0;
}

bool Router::seen(const Uri& u, Placement where) const {
    if (ws_.open_in_window(u)) return true;   // its text is in the window already
    return where == Placement::QuestionNote ? ws_.offered_this_turn(u) : ws_.offered_this_epoch(u);
}

Event Router::ev(Event::Type t, const Uri& u, const std::string& reason) const {
    Event e;
    e.type = t;
    e.ts_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    e.turn = static_cast<uint32_t>(ws_.turn());
    e.round = static_cast<uint32_t>(ws_.round());
    e.epoch = static_cast<uint32_t>(ws_.epoch());
    e.uri = u;
    e.reason = reason;
    return e;
}

void Router::end_turn() { sink_->emit(ev(Event::Type::TurnEnd)); }

void Router::note_tripwire(const std::string& rule) {
    Uri u;
    u.kind = Kind::Rule;
    u.id = rule;
    sink_->emit(ev(Event::Type::Tripwire, u));
}

void Router::drain_opened() {
    Opened o;
    bool any = false;
    while (opened_q_.pop(o)) {
        note_demand_open(o.uri);   // it was read: demand is satisfied, whatever the decayed counts still say
        ws_.opened(o.uri, o.title, o.bytes / 4);
        sink_->emit(ev(Event::Type::OpenedByModel, o.uri));
        any = true;
    }
    if (any) publish_reattach();
}

void Router::publish_reattach() {
    reattach_.publish(std::make_unique<const std::string>(Deliverer::reattach(ws_.in_use(Kind::Skill, 2))));
}

std::string Router::reattach() const {
    const auto s = reattach_.read();
    return s ? *s : std::string();
}

void Router::note_opened(const Uri& u, const std::string& title, size_t bytes) {
    opened_q_.push(Opened{u, title, bytes});
    ++stats_.opened_by_model;
}

std::optional<Body> Router::open(const Uri& u) {
    Catalogue* c = catalogue(u.kind);
    if (!c) return std::nullopt;
    auto b = c->open(u);
    if (b) note_opened(u, u.kind == Kind::Memory ? "mem " + u.id : u.id, b->total);
    return b;
}

std::vector<Hit> Router::find(const std::string& text, const std::vector<Kind>& kinds, int k) const {
    Query q = distill_.text(text, idf_fn());
    q.text = text;
    std::vector<Kind> want = kinds.empty() ? std::vector<Kind>{Kind::Skill, Kind::Memory, Kind::Tool} : kinds;
    std::vector<Hit> all;
    for (Kind kind : want)
        if (const Catalogue* c = catalogue(kind))
            for (auto& h : c->search(q, k)) all.push_back(std::move(h));
    std::stable_sort(all.begin(), all.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
    if (static_cast<int>(all.size()) > k) all.resize(static_cast<size_t>(k));
    return all;
}

context::Prefetch Router::recall_now(const std::string& query, const std::set<long>& in_window, const std::vector<long>& earlier) const {
    return recaller_.run(query, in_window, earlier);
}

std::vector<std::string> Router::map() const {
    std::vector<std::string> lines;
    for (Kind k : {Kind::Skill, Kind::Memory, Kind::Tool})
        if (const Catalogue* c = catalogue(k)) lines.push_back(c->map());
    return lines;
}

void Router::submit_deep(const std::string& query) {
    if (!pod_ || query.empty()) return;
    // one at a time within a turn, and this turn's recall that nobody has taken yet (running, or landed) is not replaced by a new
    // question; an older turn's recall is not this turn's answer: it is let go (its thread ends, its result is dropped) and a fresh one is asked
    if (inflight_ && inflight_->turn == ws_.turn() && !inflight_->delivered) return;
    auto job = std::make_shared<DeepJob>();
    job->turn = ws_.turn();
    inflight_ = job;
    const size_t chars = std::clamp<size_t>((policy_.max_turn_tokens > ws_.turn_tokens() ? policy_.max_turn_tokens - ws_.turn_tokens() : 0) * 4, 1200, 6000);
    std::thread([job, rec = recaller_, query, win = last_in_window_, earlier = last_earlier_, chars] {
        job->result = rec.run(query, win, earlier, chars);
        job->done.store(1, std::memory_order_release);
        core::unpark(job->done);
    }).detach();
}

void Router::run(const Intent& in, Delivery& d, std::vector<std::string>& notes) {
    declined_ = 0;   // the count is this signal's, not the process's
    Shown shown;
    std::vector<Hit> kept_skills;

    if (policy_.auto_route) {
        for (Kind k : in.kinds) {
            const Catalogue* cat = catalogue(k);
            if (!cat || !cat->ready()) continue;
            const Confidence& conf = k == Kind::Memory ? policy_.memory : policy_.skills;
            const size_t cap = k == Kind::Memory ? 2 : static_cast<size_t>(policy_.top_k);
            std::vector<Hit>& bucket = k == Kind::Skill ? shown.skills : k == Kind::Memory ? shown.memory : shown.tools;
            for (auto& h : cat->search(in.query, policy_.top_k + 4)) {
                if (bucket.size() >= cap) break;
                if (h.matched >= 2 && !conf.offers(h, cat->size())) {   // a near miss: the counterfactual
                    Event e = ev(Event::Type::Shadow, h.card.uri, in.reason);
                    e.score = static_cast<float>(h.score);
                    e.mass = static_cast<float>(h.mass);
                    e.matched = h.matched;
                    sink_->emit(std::move(e));
                }
                if (!conf.offers(h, cat->size()) || seen(h.card.uri, in.where)) continue;
                if (refused(h.card.uri)) continue;   // offered often, opened rarely: it does not get this slot
                if (k == Kind::Skill && !h.card.has_body) continue;   // a pointer to nothing
                bucket.push_back(std::move(h));
            }
        }
        // the best skill, when the router is sure of it: its head is opened for the model
        if (policy_.auto_open && !shown.skills.empty() && cats_.skills) {
            const Hit& top = shown.skills.front();
            if (policy_.skills.opens(top, cats_.skills->size()) && top.card.has_body && top.card.body_bytes <= policy_.open_max_bytes) {
                Uri u = top.card.uri;
                u.page.clear();
                if (auto body = cats_.skills->open(u)) {
                    shown.head = std::move(*body);
                    shown.head_title = top.card.title;
                }
            }
        }
    }
    if (in.reason == "turn" || in.reason.rfind("shift", 0) == 0) ws_.cover(in.query.terms);   // said, or looked and found nothing: the topic is covered

    const size_t left = policy_.max_turn_tokens > ws_.turn_tokens() ? policy_.max_turn_tokens - ws_.turn_tokens() : 0;
    std::string text;
    std::vector<Uri> offered;
    bool head_in = false;
    if (left > 0) text = deliver_.note(shown, in.reason, std::min(in.tokens, left), left, &offered, &head_in);
    std::string notice;
    if (in.notice) {
        notice = Deliverer::notice(in.reason.rfind("stall: same", 0) == 0 ? "repeated_call" : "blind_exploration", StallTrigger::notice_text(ws_, policy_));
        ws_.set_stalled();
        sink_->emit(ev(Event::Type::Stall, Uri(), in.reason));
    }
    if (!text.empty()) {
        ws_.spend(deliver_.tokens(text));
        for (const auto& u : offered) {
            std::string title = u.id;
            note_offer(u);
            ws_.offered(u, title, 40);
            {
                Event e = ev(Event::Type::Offered, u, in.reason);
                e.where = in.where;
                e.key = in.key;
                for (const auto& t : in.query.terms) e.terms.push_back(t.text);
                const auto& pool = u.kind == Kind::Memory ? shown.memory : u.kind == Kind::Tool ? shown.tools : shown.skills;
                for (const auto& h : pool)
                    if (h.card.uri == u) {
                        e.score = static_cast<float>(h.score);
                        e.mass = static_cast<float>(h.mass);
                        e.matched = h.matched;
                        break;
                    }
                sink_->emit(std::move(e));
            }
            d.offered.push_back(u);
            ++stats_.offered;
        }
        if (head_in) {
            Uri u = shown.skills.front().card.uri;
            note_demand_open(u);   // the router opened it for the model: that is demand satisfied, not an offer
            ws_.opened(u, shown.head_title, deliver_.tokens(shown.head->text));
            d.opened.push_back(u);
            sink_->emit(ev(Event::Type::OpenedByRouter, u, in.reason));
            ++stats_.opened_for;
            publish_reattach();
        }
        if (!shown.skills.empty() && d.skill.empty()) d.skill = shown.skills.front().card.title;
        if (!in.key.empty()) ws_.warn(in.key);
    }
    if (!notice.empty() || !text.empty()) {
        ws_.spend(deliver_.tokens(notice));
        notes.push_back(join({notice, text}, "\n"));
    }
}

Delivery Router::on(const Signal& s) {
    drain_opened();
    const auto t0 = Clock::now();
    ++stats_.signals;
    Delivery d;

    if (const auto* r = std::get_if<Rebased>(&s)) {
        ws_.new_epoch(r->epoch);
        sink_->emit(ev(Event::Type::Rebase));
        publish_reattach();
        return d;
    }
    if (std::get_if<SessionStarted>(&s)) {   // a new session, or a resumed one: nothing was said in it yet
        ws_ = WorkingSet();
        inflight_.reset();
        publish_reattach();
        return d;
    }
    if (std::get_if<Asked>(&s)) return d;
    const char* signal = "calls";
    if (const auto* t = std::get_if<TurnStarted>(&s)) {
        signal = "turn";
        ws_.new_turn(t->text);
        ws_.new_epoch(t->epoch);
        last_in_window_ = t->in_window;
        last_earlier_ = t->earlier;
        Event e = ev(Event::Type::TurnStart);
        e.key = t->anchored.empty() ? t->text.substr(0, 200) : t->anchored.substr(0, 200);
        sink_->emit(std::move(e));
    } else if (const auto* f = std::get_if<CallsFinished>(&s)) {
        ws_.new_round();
        ws_.record(f->calls);
        for (const auto& c : f->calls) {
            Event e = ev(Event::Type::Calls);
            e.call = c;
            e.key = c.error;
            sink_->emit(std::move(e));
        }
    }

    Plan plan;
    const PlanContext ctx{ws_, policy_, distill_, idf_fn()};
    for (const auto& t : triggers_) t->plan(s, ctx, plan);

    std::vector<std::string> notes;
    std::string reason;
    int wait_ms = 0;
    std::string deep_query;
    for (const auto& in : plan) {
        run(in, d, notes);
        reason += (reason.empty() ? "" : " + ") + in.reason;
        if (in.deep && deep_query.empty()) {
            deep_query = in.query.text;
            wait_ms = std::max(wait_ms, in.wait_ms);
        }
    }

    // the deep leg: the pod's recall, with a deadline. A late answer is delivered with a later round.
    const char* deep_state = "none";
    if (!deep_query.empty() && pod_ && pod_->configured()) {
        if (!pod_->up() && std::get_if<TurnStarted>(&s)) pod_->probe();
        if (pod_->up()) {
            submit_deep(deep_query);
            const bool mine = inflight_ && inflight_->turn == ws_.turn();   // never a recall an earlier turn asked
            if (mine && wait_ms > 0) {
                const auto end = Clock::now() + std::chrono::milliseconds(wait_ms);
                while (!inflight_->done.load(std::memory_order_acquire) && !(cancelled && cancelled())) {
                    const auto left = end - Clock::now();
                    if (left <= std::chrono::nanoseconds(0)) break;
                    // asleep until the recall lands (woken at once); the slice is how often a cancel is looked for
                    core::park(inflight_->done, 0, std::min<std::chrono::nanoseconds>(left, std::chrono::milliseconds(20)));
                }
            }
            if (mine && inflight_->done.load(std::memory_order_acquire) && !inflight_->delivered) {
                inflight_->delivered = true;
                d.deep = inflight_->result;
                if (!d.deep.block.empty()) {
                    ws_.spend(deliver_.tokens(d.deep.block));
                    notes.push_back(d.deep.block);
                }
                ++stats_.deep_in;
                deep_state = "in";
            } else if (mine && !inflight_->delivered) {
                d.deep_late = true;
                deep_state = "late";
            }
        }
    }

    d.note = join(notes, "\n\n");
    d.ms = ms_since(t0);
    // the recall card's one line
    std::vector<std::string> bits;
    if (std::strcmp(signal, "calls") == 0 && !reason.empty()) bits.push_back(reason.size() > 60 ? utf8_head(reason, 60) + "…" : reason);   // why it spoke mid-turn
    if (!d.opened.empty()) bits.push_back("opened " + d.opened.front().id);
    if (!d.skill.empty() && d.opened.empty()) bits.push_back("skill " + d.skill);
    if (d.offered.size() > (d.skill.empty() ? 0u : 1u)) bits.push_back(std::to_string(d.offered.size()) + " pointers");
    if (!d.deep.block.empty() || d.deep.hits) {
        bits.push_back(std::to_string(d.deep.hits) + " recalled");
        bits.push_back(std::to_string(d.deep.linked) + " linked");
        bits.push_back(std::to_string(d.deep.facts) + " facts");
        if (d.deep.earlier) bits.push_back(std::to_string(d.deep.earlier) + " earlier");
        if (!d.deep.intent.empty()) bits.push_back(d.deep.intent);
    }
    if (d.deep_late) bits.push_back("deep recall pending");
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.1fs", d.ms / 1000.0);
    bits.push_back(buf);
    d.summary = join(bits, " · ");
    if (!d.note.empty()) ++stats_.notes;
    stats_.tokens += deliver_.tokens(d.note);
    if (!d.note.empty() || d.deep_late) log(signal, d, reason, deep_state);
    return d;
}

std::optional<Delivery> Router::late() {
    drain_opened();
    if (!inflight_ || !inflight_->done.load(std::memory_order_acquire) || inflight_->delivered) return std::nullopt;
    inflight_->delivered = true;
    if (inflight_->turn != ws_.turn() || inflight_->result.block.empty()) return std::nullopt;   // a past turn's, or nothing
    const size_t left = policy_.max_turn_tokens > ws_.turn_tokens() ? policy_.max_turn_tokens - ws_.turn_tokens() : 0;
    if (left < 100) return std::nullopt;
    Delivery d;
    d.deep = inflight_->result;
    d.note = d.deep.block;
    ws_.spend(deliver_.tokens(d.note));
    ++stats_.deep_late;
    ++stats_.notes;
    stats_.tokens += deliver_.tokens(d.note);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.1fs", d.deep.ms / 1000.0);
    d.summary = std::to_string(d.deep.hits) + " recalled · " + std::to_string(d.deep.linked) + " linked · " + std::to_string(d.deep.facts) +
                " facts · late · " + buf;
    log("late", d, "late recall", "late");
    return d;
}

void Router::log(const char* signal, const Delivery& d, const std::string& reason, const char* deep) const {
    if (!on_log) return;
    using core::Json;
    auto sp = [](Json v) { return std::make_shared<Json>(std::move(v)); };
    Json offered = core::json_arr(), opened = core::json_arr();
    for (const auto& u : d.offered) core::arr_push(offered, core::json_str(u.str()));
    for (const auto& u : d.opened) core::arr_push(opened, core::json_str(u.str()));
    on_log(core::json_dump(core::json_obj({{"t", sp(core::json_num(static_cast<double>(std::time(nullptr))))},
                                           {"signal", sp(core::json_str(signal))},
                                           {"reason", sp(core::json_str(utf8_head(reason, 120)))},
                                           {"offered", sp(std::move(offered))},
                                           {"opened", sp(std::move(opened))},
                                           {"declined", sp(core::json_num(static_cast<double>(declined_)))},
                                           {"tokens", sp(core::json_num(static_cast<double>(deliver_.tokens(d.note))))},
                                           {"deep", sp(core::json_str(deep))},
                                           {"ms", sp(core::json_num(d.ms))}})));
}

}  // namespace know
}  // namespace dd
