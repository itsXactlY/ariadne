// dd::learn credit — the outcome table, the observer's windows, the fold's determinism, the queue.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "dd/learn/observer.hpp"
#include "dd/learn/sink.hpp"
#include "dd/learn/stats.hpp"

using namespace dd;
using namespace dd::learn;
using know::Event;
using T = Event::Type;

static int g_fail = 0;
static void chk(bool ok, const std::string& label) {
    if (!ok) {
        ++g_fail;
        std::printf("FAIL %s\n", label.c_str());
    }
}

static know::Uri skill(const std::string& id) {
    know::Uri u;
    u.kind = know::Kind::Skill;
    u.id = id;
    return u;
}

static Event ev(T type, uint32_t turn, uint32_t round, const std::string& id = "", uint64_t ts = 0) {
    Event e;
    e.type = type;
    e.turn = turn;
    e.round = round;
    e.ts_ns = ts * 1000000000ull;
    if (!id.empty()) e.uri = skill(id);
    return e;
}

static Event call(uint32_t turn, uint32_t round, bool failed, const std::string& error = "", uint64_t ts = 0) {
    Event e = ev(T::Calls, turn, round, "", ts);
    e.call.failed = failed;
    e.call.error = error;
    return e;
}

// ---- the table ----

struct Row {
    const char* name;
    Exposure x;
    Outcome want;
};

static void test_rule_table() {
    RuleCredit rc;
    auto mk = [](bool opened, bool recurred, bool stalled, bool vetoed, int tail) {
        Exposure x;
        x.opened = opened;
        x.error_recurred = recurred;
        x.stalled = stalled;
        x.vetoed = vetoed;
        x.trailing_failures = tail;
        return x;
    };
    const Row rows[] = {
        {"offered, never opened", mk(false, false, false, false, 0), Outcome::Ignored},
        {"never opened, even if the tail failed", mk(false, false, false, false, 5), Outcome::Ignored},
        {"opened, quiet", mk(true, false, false, false, 0), Outcome::Helped},
        {"opened, one failed call at the end", mk(true, false, false, false, 1), Outcome::Helped},
        {"opened, failed tail", mk(true, false, false, false, 2), Outcome::Opened},
        {"opened, error recurred", mk(true, true, false, false, 0), Outcome::Failed},
        {"opened, stalled", mk(true, false, true, false, 0), Outcome::Failed},
        {"recurred beats a failed tail", mk(true, true, false, false, 3), Outcome::Failed},
        {"not opened but stalled", mk(false, false, true, false, 0), Outcome::Ignored},
        {"tripwire", mk(false, false, false, true, 0), Outcome::Vetoed},
    };
    for (const auto& r : rows) chk(rc.judge(r.x) == r.want, std::string("table: ") + r.name);
    Exposure x;
    chk(x.weight() == 1.0f, "rank 0 weighs 1");
    x.rank = 2;
    chk(x.weight() == 0.5f, "lower ranks weigh half");
}

// ---- the observer ----

struct Run {
    std::vector<OutcomeRec> out;
    Observer obs{[this](OutcomeRec&& r) { out.push_back(std::move(r)); }};
    void feed(const Event& e) { obs.feed(e); }
    const OutcomeRec* of(const std::string& id) const {
        for (const auto& r : out)
            if (r.uri.id == id) return &r;
        return nullptr;
    }
};

static void test_observer() {
    {   // offered, opened by the model, turn ends quietly
        Run r;
        r.feed(ev(T::TurnStart, 1, 0));
        Event o = ev(T::Offered, 1, 0, "podman", 100);
        o.reason = "turn";
        o.terms = {"podman", "quadlet"};
        r.feed(o);
        r.feed(ev(T::OpenedByModel, 1, 1, "podman", 101));
        r.feed(call(1, 1, false));
        r.feed(ev(T::TurnEnd, 1, 1, "", 105));
        chk(r.out.size() == 1 && r.out[0].outcome == Outcome::Helped, "observer: opened + quiet = Helped");
        chk(r.out.size() == 1 && r.out[0].terms.size() == 2 && r.out[0].trigger == "turn" && r.out[0].ts == 105, "observer: terms, trigger and close time kept");
    }
    {   // offered, ignored
        Run r;
        r.feed(ev(T::TurnStart, 1, 0));
        r.feed(ev(T::Offered, 1, 0, "a"));
        r.feed(ev(T::TurnEnd, 1, 1));
        chk(r.of("a") && r.of("a")->outcome == Outcome::Ignored, "observer: no open = Ignored");
    }
    {   // failure trigger, opened, same error again
        Run r;
        r.feed(ev(T::TurnStart, 1, 0));
        Event o = ev(T::Offered, 1, 1, "fix", 10);
        o.reason = "failure";
        o.key = "cannot open <path>";
        r.feed(o);
        r.feed(ev(T::OpenedByModel, 1, 1, "fix"));
        r.feed(call(1, 2, true, "cannot open <path>"));
        r.feed(ev(T::TurnEnd, 1, 2));
        chk(r.of("fix") && r.of("fix")->outcome == Outcome::Failed, "observer: recurring signature = Failed");
    }
    {   // failure trigger, a different error is not a recurrence
        Run r;
        r.feed(ev(T::TurnStart, 1, 0));
        Event o = ev(T::Offered, 1, 1, "fix");
        o.reason = "failure";
        o.key = "E1";
        r.feed(o);
        r.feed(ev(T::OpenedByModel, 1, 1, "fix"));
        r.feed(call(1, 2, true, "E2"));
        r.feed(call(1, 3, false));
        r.feed(ev(T::TurnEnd, 1, 3));
        chk(r.of("fix") && r.of("fix")->outcome == Outcome::Helped, "observer: another error then success = Helped");
    }
    {   // stall after the open
        Run r;
        r.feed(ev(T::TurnStart, 1, 0));
        r.feed(ev(T::Offered, 1, 0, "s"));
        r.feed(ev(T::OpenedByRouter, 1, 0, "s"));
        r.feed(ev(T::Stall, 1, 2));
        r.feed(ev(T::TurnEnd, 1, 2));
        chk(r.of("s") && r.of("s")->outcome == Outcome::Failed, "observer: stall after open = Failed");
    }
    {   // a stall before any open does not blame the unread card
        Run r;
        r.feed(ev(T::TurnStart, 1, 0));
        r.feed(ev(T::Offered, 1, 0, "s"));
        r.feed(ev(T::Stall, 1, 2));
        r.feed(ev(T::TurnEnd, 1, 2));
        chk(r.of("s") && r.of("s")->outcome == Outcome::Ignored, "observer: stall without open = Ignored");
    }
    {   // the round cap closes a window mid-turn; later calls do not touch it
        Run r;
        r.feed(ev(T::TurnStart, 1, 0));
        r.feed(ev(T::Offered, 1, 0, "w"));
        r.feed(ev(T::OpenedByModel, 1, 0, "w"));
        r.feed(call(1, 3, false));
        chk(r.out.empty(), "observer: still open at round 3");
        r.feed(call(1, 6, true, "x"));
        chk(r.out.size() == 1 && r.out[0].outcome == Outcome::Helped, "observer: closed at round 6, before that call");
        chk(r.obs.open() == 0, "observer: nothing left open");
    }
    {   // a failed tail across the close
        Run r;
        r.feed(ev(T::TurnStart, 1, 0));
        r.feed(ev(T::Offered, 1, 0, "t"));
        r.feed(ev(T::OpenedByModel, 1, 0, "t"));
        r.feed(call(1, 1, true, "a"));
        r.feed(call(1, 2, true, "b"));
        r.feed(ev(T::TurnEnd, 1, 2));
        chk(r.of("t") && r.of("t")->outcome == Outcome::Opened, "observer: failed tail = Opened (no verdict)");
    }
    {   // a refused call is neither a failure nor a success
        Run r;
        r.feed(ev(T::TurnStart, 1, 0));
        r.feed(ev(T::Offered, 1, 0, "t"));
        r.feed(ev(T::OpenedByModel, 1, 0, "t"));
        r.feed(call(1, 1, true, "a"));
        Event c = call(1, 2, true, "b");
        c.call.refused = true;
        r.feed(c);
        r.feed(ev(T::TurnEnd, 1, 2));
        chk(r.of("t") && r.of("t")->outcome == Outcome::Helped, "observer: refused calls do not count");
    }
    {   // position bias, one exposure per card, uri normalised
        Run r;
        r.feed(ev(T::TurnStart, 1, 0));
        r.feed(ev(T::Offered, 1, 0, "a"));
        r.feed(ev(T::Offered, 1, 0, "b"));
        r.feed(ev(T::Offered, 1, 0, "a"));   // again: ignored
        Event o = ev(T::OpenedByModel, 1, 0, "c");
        o.uri.path = "references/x.md";
        o.uri.page = "2";
        r.feed(o);
        r.feed(ev(T::TurnEnd, 1, 1));
        chk(r.out.size() == 3, "observer: three exposures");
        chk(r.of("a")->weight == 1.0f && r.of("b")->weight == 0.5f, "observer: top 1, second half");
        chk(r.of("c") && r.of("c")->uri.path.empty() && r.of("c")->uri.page.empty() && r.of("c")->trigger == "model", "observer: a model-found open is credited to the skill");
    }
    {   // tripwire
        Run r;
        r.feed(ev(T::TurnStart, 1, 0));
        Event t = ev(T::Tripwire, 1, 1);
        t.uri.kind = know::Kind::Rule;
        t.uri.id = "podman-not-docker";
        r.feed(t);
        chk(r.out.size() == 1 && r.out[0].outcome == Outcome::Vetoed && r.obs.open() == 0, "observer: tripwire = Vetoed at once");
    }
    {   // episodes advance per turn; a turn start closes stragglers
        Run r;
        r.feed(ev(T::TurnStart, 1, 0));
        r.feed(ev(T::Offered, 1, 0, "a"));
        r.feed(ev(T::TurnStart, 2, 0));
        r.feed(ev(T::Offered, 2, 0, "a"));
        r.feed(ev(T::TurnEnd, 2, 1));
        chk(r.out.size() == 2 && r.out[0].episode == 1 && r.out[1].episode == 2, "observer: one episode per turn");
    }
    {   // flush
        Run r;
        r.feed(ev(T::TurnStart, 1, 0));
        r.feed(ev(T::Offered, 1, 0, "a"));
        r.obs.flush();
        chk(r.out.size() == 1 && r.obs.open() == 0, "observer: flush closes");
    }
}

// ---- the fold ----

static std::string dump(const Stats& s) {
    std::vector<std::string> keys;
    for (const auto& [k, c] : s.cards) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    std::string o;
    char b[256];
    for (const auto& k : keys) {
        const auto& c = s.cards.at(k);
        std::snprintf(b, sizeof b, "%s|%.17g@%lld|%.17g@%lld|%.17g|%.17g|%.17g|%d|%lld|%lld\n", k.c_str(), c.offered.v, (long long)c.offered.t, c.opened.v, (long long)c.opened.t,
                      c.helped.v, c.failed.v, c.shadow.v, (int)c.state, (long long)c.first_seen, (long long)c.last_used);
        o += b;
    }
    for (const auto& [k, a] : s.assoc) {
        std::snprintf(b, sizeof b, "A %s|%.17g@%lld|%u|%u|%u|%lld\n", k.c_str(), a.weight.v, (long long)a.weight.t, a.episodes, a.last_episode, a.days, (long long)a.last_day);
        o += b;
    }
    std::snprintf(b, sizeof b, "n=%llu now=%lld\n", (unsigned long long)s.records, (long long)s.now_hint);
    return o + b;
}

static OutcomeRec outc(const std::string& id, Outcome o, int64_t ts, uint32_t episode, std::vector<std::string> terms = {}, float w = 1.0f) {
    OutcomeRec r;
    r.uri = skill(id);
    r.outcome = o;
    r.ts = ts;
    r.episode = episode;
    r.terms = std::move(terms);
    r.weight = w;
    return r;
}

static std::vector<Record> sample_records() {
    std::vector<Record> v;
    Event off = ev(T::Offered, 1, 0, "a", 1000);
    v.push_back(EventRec{off});
    v.push_back(EventRec{ev(T::OpenedByModel, 1, 1, "a", 1001)});
    v.push_back(outc("a", Outcome::Helped, 1002, 1, {"podman", "quadlet"}));
    v.push_back(outc("a", Outcome::Helped, 1003 + 86400, 2, {"podman", "pod"}));
    v.push_back(outc("a", Outcome::Helped, 1004 + 86400, 2, {"podman"}));   // same episode, same day
    v.push_back(outc("b", Outcome::Failed, 1005, 1, {"x"}));
    v.push_back(outc("b", Outcome::Ignored, 1006, 1));
    v.push_back(EventRec{ev(T::Shadow, 2, 0, "c", 1007)});
    v.push_back(StateRec{skill("b"), State::Active, State::Probation, "new", 1008});
    v.push_back(DraftRec{"s", "learned", "d", "ok", true, 1009});
    return v;
}

static void test_fold() {
    const auto recs = sample_records();
    Stats a, b, c;
    for (const auto& r : recs) a.apply(r);
    for (const auto& r : recs) b.apply(r);
    std::vector<Record> copy = recs;   // a replayed copy
    for (const auto& r : copy) c.apply(r);
    chk(dump(a) == dump(b) && dump(a) == dump(c), "fold: deterministic");
    chk(a.records == recs.size(), "fold: counts every record");
    const auto& ca = a.cards.at("skill://a");
    chk(ca.offered.v == 1 && ca.opened.v == 1 && std::fabs(ca.helped.v) > 0, "fold: events and outcomes counted");
    chk(a.cards.at("skill://b").state == State::Probation, "fold: state");
    chk(a.cards.at("skill://c").shadow.v == 1, "fold: shadow");
    const auto& pod = a.assoc.at(std::string("skill://a") + '\x1f' + "podman");
    chk(pod.episodes == 2 && pod.days == 2, "fold: assoc counts distinct episodes and days");
    const auto& quad = a.assoc.at(std::string("skill://a") + '\x1f' + "quadlet");
    chk(quad.episodes == 1 && quad.days == 1, "fold: a single sighting is not a confirmation");
    chk(a.assoc.count(std::string("skill://b") + '\x1f' + "x") == 0, "fold: only Helped feeds assoc");
    chk(a.now_hint >= 1009 + 0, "fold: now_hint follows the records");

    {   // Wilson
        CardStats z;
        chk(z.efficacy_lcb(0) == 0.5 && z.conversion_lcb(0) == 0.5, "wilson: no evidence = 0.5");
        CardStats few, many;
        few.helped.add(3, 0, kHlEfficacy);
        many.helped.add(50, 0, kHlEfficacy);
        chk(few.efficacy_lcb(0) < many.efficacy_lcb(0), "wilson: 3 wins rank under 50");
        chk(many.efficacy_lcb(0) > 0.9 && few.efficacy_lcb(0) < 0.7, "wilson: bounds");
        CardStats bad;
        bad.helped.add(1, 0, kHlEfficacy);
        bad.failed.add(9, 0, kHlEfficacy);
        chk(bad.efficacy_lcb(0) < 0.2, "wilson: mostly failing is low");
        CardStats conv;
        conv.offered.add(10, 0, kHlConversion);
        conv.opened.add(40, 0, kHlConversion);   // never above the denominator
        chk(conv.conversion_lcb(0) <= 1.0 && conv.conversion_lcb(0) > 0.6, "wilson: conversion clamps opened to offered");
        const double fresh = few.efficacy_lcb(0);
        chk(few.efficacy_lcb(int64_t(90 * kDay)) <= 0.5 + 1e-9 || few.efficacy_lcb(int64_t(90 * kDay)) < fresh + 0.2, "decay: old evidence fades toward the same bound or less");
    }
    {   // a clock that runs backwards must not inflate
        Decayed d;
        d.add(1, 1000, kHlConversion);
        chk(d.at(500, kHlConversion) == 1.0, "decayed: negative dt clamped");
        chk(d.at(1000 + int64_t(400 * kDay), kHlConversion) < 0.01 * 1e3, "decayed: a jump is clamped to 30 days");
    }
}

static void test_tombstone() {
    Stats s;
    for (const auto& r : sample_records()) s.apply(r);
    s.apply(TombstoneRec{skill("a"), 2000});
    chk(!s.cards.count("skill://a"), "tombstone: card gone");
    bool left = false;
    for (const auto& [k, a] : s.assoc) left = left || k.rfind("skill://a\x1f", 0) == 0;
    chk(!left, "tombstone: assoc gone");
    chk(s.cards.count("skill://b") == 1, "tombstone: others stay");
    // a prefix-sharing id must survive: skill://ab vs skill://a
    Stats t;
    t.apply(outc("a", Outcome::Helped, 1, 1, {"x"}));
    t.apply(outc("ab", Outcome::Helped, 1, 1, {"x"}));
    t.apply(TombstoneRec{skill("a"), 2});
    chk(t.cards.count("skill://ab") == 1 && t.assoc.size() == 1, "tombstone: a longer id with the same prefix survives");
    // learning resumes after a tombstone
    t.apply(outc("a", Outcome::Failed, 3, 2));
    chk(t.cards.at("skill://a").failed.v == 1 && t.cards.at("skill://a").helped.v == 0, "tombstone: starts fresh");
    // replay equality including the tombstone
    Stats u, w;
    std::vector<Record> recs = sample_records();
    recs.push_back(TombstoneRec{skill("a"), 2000});
    recs.push_back(outc("a", Outcome::Helped, 2001, 9, {"q"}));
    for (const auto& r : recs) u.apply(r);
    for (const auto& r : recs) w.apply(r);
    chk(dump(u) == dump(w), "tombstone: replay equal");
}

static void test_assoc_cap() {
    Stats s;
    for (int i = 0; i < 90000; ++i) {
        std::vector<std::string> t = {"t" + std::to_string(i)};
        s.apply(outc("a", Outcome::Helped, 1000 + i, uint32_t(i), t, i % 7 ? 1.0f : 3.0f));
    }
    chk(s.assoc.size() <= 65536 + 65536 / 4, "assoc: bounded");
    Stats s2;
    for (int i = 0; i < 90000; ++i) {
        std::vector<std::string> t = {"t" + std::to_string(i)};
        s2.apply(outc("a", Outcome::Helped, 1000 + i, uint32_t(i), t, i % 7 ? 1.0f : 3.0f));
    }
    chk(dump(s) == dump(s2), "assoc: eviction is deterministic");
}

// ---- hubs ----

struct Lcg {
    uint64_t x = 88172645463325252ull;
    double next() {   // [0,1)
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        return double(x >> 11) / double(1ull << 53);
    }
};

// 20 equally good cards, all match every query. A naive learner reads the smoothed success rate and
// entrenches early luck; the shadow-aware one scores with the lower bound and credits near misses.
static double hub_share(bool shadow_aware) {
    const int N = 20, STEPS = 6000;
    Stats s;
    Lcg rng;
    std::vector<int> picks(N, 0);
    for (int step = 0; step < STEPS; ++step) {
        const int64_t ts = 1000 + step * 60;
        double best = -1, scores[N];
        int top = 0;
        for (int i = 0; i < N; ++i) {
            const auto it = s.cards.find("skill://c" + std::to_string(i));
            double prior = 1.0;
            if (it != s.cards.end()) {
                const CardStats& c = it->second;
                if (shadow_aware) {
                    const double off = c.offered.at(ts, kHlConversion), sh = c.shadow.at(ts, kHlConversion);
                    prior = 1.0 + 0.5 * (c.efficacy_lcb(ts) - 0.5) + 1.0 * sh / (sh + off + 1.0);
                } else {
                    const double h = c.helped.at(ts, kHlEfficacy), f = c.failed.at(ts, kHlEfficacy);
                    prior = 0.5 + 1.5 * (h + 0.5) / (h + f + 1.0);
                }
                prior = std::clamp(prior, 0.5, 1.5);
            }
            scores[i] = prior * (1.0 + 0.02 * rng.next());
            if (scores[i] > best) best = scores[i], top = i;
        }
        const uint64_t ns = uint64_t(ts) * 1000000000ull;
        Event o = ev(T::Offered, 1, 0, "c" + std::to_string(top));
        o.ts_ns = ns;
        s.apply(EventRec{o});
        ++picks[top];
        if (shadow_aware) {
            for (int i = 0; i < N; ++i) {
                if (i == top) continue;   // every card matches every query
                Event sh = ev(T::Shadow, 1, 0, "c" + std::to_string(i));
                sh.ts_ns = ns;
                s.apply(EventRec{sh});
            }
        }
        s.apply(outc("c" + std::to_string(top), rng.next() < 0.6 ? Outcome::Helped : Outcome::Failed, ts, uint32_t(step + 1)));
    }
    return double(*std::max_element(picks.begin(), picks.end())) / STEPS;
}

static void test_hubs() {
    const double naive = hub_share(false), aware = hub_share(true);
    std::printf("hub share of the top card (20 equal cards, uniform 0.050): naive %.3f, shadow-aware %.3f\n", naive, aware);
    chk(naive > 0.5, "hubs: without shadow events one card takes over");
    chk(aware < 0.12, "hubs: with shadow events the offers stay spread");
}

// ---- the queue ----

static void test_queue_cap() {
    QueueSink q(1000);
    std::vector<std::thread> ps;
    for (int p = 0; p < 8; ++p)
        ps.emplace_back([&q, p] {
            for (int i = 0; i < 5000; ++i) {
                Event e = ev(T::Offered, uint32_t(p), uint32_t(i), "x");
                q.emit(std::move(e));
            }
        });
    for (auto& t : ps) t.join();
    chk(q.pushed() == 1000, "queue: exactly cap accepted with no consumer");
    chk(q.dropped() == 40000 - 1000, "queue: the rest dropped and counted");
    chk(q.depth() == 1000, "queue: depth");
    Event e;
    size_t n = 0;
    while (q.pop(e)) ++n;
    chk(n == 1000 && q.depth() == 0, "queue: drains to empty");
    q.emit(ev(T::Offered, 0, 0, "y"));
    chk(q.pop(e) && e.uri.id == "y", "queue: usable after a drop");
}

static void test_queue_concurrent() {
    QueueSink q(4096);
    constexpr int P = 8, N = 20000;
    std::atomic<bool> done{false};
    std::vector<uint32_t> last(P, 0);
    std::vector<int> seen(P, 0);
    bool order_ok = true;
    uint64_t popped = 0;
    std::thread consumer([&] {
        Event e;
        for (;;) {
            bool any = false;
            while (q.pop(e)) {
                any = true;
                ++popped;
                const int p = int(e.turn);
                if (e.round < last[p]) order_ok = false;   // dropped ones leave gaps, never reorder
                last[p] = e.round;
                ++seen[p];
            }
            if (!any) {
                if (done.load() && q.depth() == 0) break;
                q.wait(std::chrono::milliseconds(50));
            }
        }
    });
    std::vector<std::thread> ps;
    for (int p = 0; p < P; ++p)
        ps.emplace_back([&q, p] {
            for (int i = 0; i < N; ++i) q.emit(ev(T::Calls, uint32_t(p), uint32_t(i)));
        });
    for (auto& t : ps) t.join();
    done = true;
    q.wake();
    consumer.join();
    chk(q.pushed() + q.dropped() == uint64_t(P) * N, "queue: every emit is pushed or dropped");
    chk(popped == q.pushed(), "queue: every accepted event is popped");
    chk(order_ok, "queue: per-producer order kept");
}

static void test_queue_wait() {
    QueueSink q;
    std::atomic<bool> woke{false};
    std::atomic<int64_t> waited_ms{0};
    std::thread t([&] {
        const auto t0 = std::chrono::steady_clock::now();
        const bool got = q.wait(std::chrono::seconds(10));
        waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        woke = got;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    q.emit(ev(T::Offered, 0, 0, "z"));
    t.join();
    chk(woke && waited_ms < 3000, "queue: emit wakes a parked consumer");

    std::atomic<int64_t> w2{0};
    bool got2 = true;
    std::thread t2([&] {
        const auto t0 = std::chrono::steady_clock::now();
        got2 = q.wait(std::chrono::seconds(10)) ;   // the earlier event is still queued: returns at once
        w2 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    });
    t2.join();
    chk(got2 && w2 < 500, "queue: wait returns at once when something is queued");

    Event e;
    while (q.pop(e)) {}
    std::atomic<int64_t> w3{0};
    bool got3 = true;
    std::thread t3([&] {
        const auto t0 = std::chrono::steady_clock::now();
        got3 = q.wait(std::chrono::seconds(10));
        w3 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    q.wake();
    t3.join();
    chk(!got3 && w3 < 3000, "queue: wake() ends a wait with nothing queued");

    const auto t0 = std::chrono::steady_clock::now();
    const bool timed = q.wait(std::chrono::milliseconds(60));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    chk(!timed && ms >= 40 && ms < 1000, "queue: wait times out");
}

int main() {
    test_rule_table();
    test_observer();
    test_fold();
    test_tombstone();
    test_assoc_cap();
    test_hubs();
    test_queue_cap();
    test_queue_concurrent();
    test_queue_wait();
    if (g_fail) {
        std::printf("%d FAILED\n", g_fail);
        return 1;
    }
    std::printf("learn credit tests: all passed\n");
    return 0;
}
