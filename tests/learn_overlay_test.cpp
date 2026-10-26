// dd::learn read side — Affinity promotion, the Curator's clamp and step, the version that stays still on noise, the parallel fold
// equal at every thread count, and the overlay reaching a SkillCatalogue's ranking.
#include <omp.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "dd/know/skills.hpp"
#include "dd/learn/affinity.hpp"
#include "dd/learn/curator.hpp"
#include "dd/learn/overlay.hpp"

using namespace dd;
using namespace dd::learn;

static int g_fail = 0;
static void chk(bool ok, const std::string& label) {
    if (!ok) {
        ++g_fail;
        if (g_fail <= 12) std::printf("FAIL %s\n", label.c_str());
    }
}

static const int64_t kNow = 1'800'000'000;

static know::Uri skill(const std::string& id) {
    know::Uri u;
    u.kind = know::Kind::Skill;
    u.id = id;
    return u;
}

static void add_assoc(Stats& s, const std::string& card, const std::string& term, uint32_t episodes, uint16_t days, double w = 3.0) {
    Assoc& a = s.assoc[assoc_key(skill(card).str(), term)];
    a.episodes = episodes;
    a.days = days;
    a.weight.add(w, kNow, kHlAssoc);
}

static void give_efficacy(Stats& s, const std::string& card, double helped, double failed) {
    CardStats& c = s.cards[skill(card).str()];
    c.helped.add(helped, kNow, kHlEfficacy);
    c.failed.add(failed, kNow, kHlEfficacy);
}

static void test_demand_rows() {
    // Demand is published every pass and must never bump the version: decayed counts change constantly, and a
    // version bump rebuilds the catalogues. The router reads demand; the cards do not change.
    Stats s;
    CardStats& c = s.cards[skill("hub-no-open").str()];
    for (int i = 0; i < 10; ++i) c.offered.add(1, kNow, kHlConversion);
    auto ov = std::make_shared<LearnOverlay>();
    Curator cur(ov);
    cur.pass(s, kNow);   // nothing moves a prior here: the demand row must land regardless
    const auto d = ov->demand(skill("hub-no-open"));
    chk(d.offered == 10.0 && d.opened == 0.0 && d.lcb < 0.15, "demand: ten offers with none opened reads as refused");
    chk(ov->version() == 0, "demand: demand alone does not move the version (no catalogue rebuild)");
    chk(ov->demand(skill("never-offered")).offered == 0.0, "demand: a card nobody offered has no row");

    // and the curator no longer boosts a card it offered a lot: its own demand says stop
    CardStats& hub = s.cards[skill("hub-no-open").str()];
    hub.shadow.add(5, kNow, kHlConversion);   // near misses that were not offered
    cur.pass(s, kNow + 60);
    chk(ov->prior(skill("hub-no-open")) <= 1.0 + 1e-9, "demand: a boosted prior needs a card nobody offered");
}

static void test_affinity_promotion() {
    Stats s;
    add_assoc(s, "podman-ops", "ship", 3, 2);      // confirmed
    add_assoc(s, "podman-ops", "rollout", 2, 5);   // too few episodes
    add_assoc(s, "podman-ops", "deploy", 9, 1);    // one day only
    add_assoc(s, "kernel", "tune", 4, 3, 1.0);
    const auto got = promote(s, kNow);
    chk(got.size() == 2, "two cards have promoted terms");
    chk(got.at(skill("podman-ops").str()).size() == 1 && got.at(skill("podman-ops").str())[0].text == "ship", "only the confirmed pair is promoted");

    Stats big;
    for (int i = 0; i < 40; ++i) add_assoc(big, "hub", "t" + std::to_string(i), 5, 4, 1.0 + i);
    const auto cap = promote(big, kNow);
    const auto& terms = cap.at(skill("hub").str());
    chk(terms.size() == 24, "a card keeps at most 24 terms");
    chk(terms.front().text == "t39" && terms.back().text == "t16", "the lowest decayed weights are evicted");

    Stats old;   // a faint weight is under the floor
    add_assoc(old, "x", "faded", 5, 4, 0.02);
    AffinityParams p;
    p.min_weight = 0.05;
    chk(promote(old, kNow, p).empty(), "a faded pair is forgotten");
}

static void test_clamp_and_step() {
    auto ov = std::make_shared<LearnOverlay>();
    Curator cur(ov);
    Stats s;
    give_efficacy(s, "good", 400, 0);
    give_efficacy(s, "bad", 0, 400);
    chk(cur.pass(s, kNow), "the first pass publishes");
    const double g1 = ov->prior(skill("good")), b1 = ov->prior(skill("bad"));
    chk(g1 > 1.0 && g1 <= 1.05 + 1e-9, "a good card moves up by at most 5% in one pass");
    chk(b1 < 1.0 && b1 >= 0.95 - 1e-9, "a bad card moves down by at most 5% in one pass");
    for (int i = 0; i < 60; ++i) cur.pass(s, kNow);
    chk(ov->prior(skill("good")) <= 1.5 + 1e-9 && ov->prior(skill("good")) > 1.4, "a good card settles under the ceiling");
    chk(ov->prior(skill("bad")) >= 0.5 - 1e-9 && ov->prior(skill("bad")) < 0.6, "a bad card settles over the floor");
    chk(ov->prior(skill("unknown")) == 1.0, "an unknown card is neutral");

    Stats prob;
    give_efficacy(prob, "new", 400, 0);
    prob.cards[skill("new").str()].state = State::Probation;
    auto ov2 = std::make_shared<LearnOverlay>();
    Curator c2(ov2);
    chk(c2.pass(prob, kNow), "a demotion publishes");
    const double p1 = ov2->prior(skill("new"));
    chk(std::fabs(p1 - 0.95) < 1e-9, "a demotion costs one step, not the whole cap");
    chk(p1 > 0.7, "the cap is not reached in the pass the demotion landed");
    c2.pass(prob, kNow);
    const double p2 = ov2->prior(skill("new"));
    chk(p2 < p1 && p1 - p2 <= 0.05 * p1 + 1e-9, "the descent toward the cap is rate-limited");
    for (int i = 0; i < 60; ++i) c2.pass(prob, kNow);
    chk(ov2->prior(skill("new")) <= 0.7 + 1e-9, "a card on probation is held under its cap");

    // terminal states are not noise: a card that is gone must stop being offered in this pass
    Stats gone;
    give_efficacy(gone, "dead", 400, 0);
    gone.cards[skill("dead").str()].state = State::Retired;
    auto ov3 = std::make_shared<LearnOverlay>();
    Curator c3(ov3);
    chk(c3.pass(gone, kNow) && ov3->prior(skill("dead")) == 0.5, "a retired card pins to the floor at once");
}

static void test_version_stable_on_noise() {
    auto ov = std::make_shared<LearnOverlay>();
    Curator cur(ov);
    Stats s;
    give_efficacy(s, "a", 30, 2);
    give_efficacy(s, "b", 10, 10);
    add_assoc(s, "a", "ship", 3, 2);
    chk(cur.pass(s, kNow), "published once");
    for (int i = 0; i < 40; ++i) cur.pass(s, kNow);   // settle
    const uint64_t v = ov->version();
    chk(!cur.pass(s, kNow) && ov->version() == v, "no change: no publish, same version");
    s.cards[skill("a").str()].helped.add(0.01, kNow, kHlEfficacy);   // noise
    chk(!cur.pass(s, kNow) && ov->version() == v, "noise under the threshold: same version");
    add_assoc(s, "b", "rollout", 3, 2);
    chk(cur.pass(s, kNow) && ov->version() == v + 1, "a newly promoted term bumps the version once");
}

static Stats corpus(size_t n) {
    Stats s;
    for (size_t i = 0; i < n; ++i) {
        const std::string id = "card-" + std::to_string(i);
        give_efficacy(s, id, static_cast<double>(i % 17), static_cast<double>(i % 5));
        CardStats& c = s.cards[skill(id).str()];
        c.shadow.add(static_cast<double>(i % 7), kNow, kHlConversion);
        if (i % 11 == 0) c.state = State::Probation;
        if (i % 13 == 0) add_assoc(s, id, "term" + std::to_string(i % 29), 3 + i % 4, 2 + i % 3, 1.0 + static_cast<double>(i % 9));
    }
    return s;
}

static void test_parallel_equals_serial_and_timing() {
    const Stats s = corpus(1000);
    CuratorParams p;
    p.par_cards = 1;
    std::shared_ptr<const OverlaySnapshot> ref;
    for (int threads : {1, 2, 4, 8}) {
        omp_set_num_threads(threads);
        auto ov = std::make_shared<LearnOverlay>();
        Curator cur(ov, p);
        for (int i = 0; i < 3; ++i) cur.pass(s, kNow);
        const auto snap = ov->copy();
        if (!ref) ref = snap;
        chk(snap->prior == ref->prior, "priors at " + std::to_string(threads) + " threads equal the one-thread priors");
        bool same = snap->terms.size() == ref->terms.size();
        for (const auto& [k, t] : snap->terms) {
            const auto it = ref->terms.find(k);
            same = same && it != ref->terms.end() && it->second.size() == t.size();
            for (size_t i = 0; same && i < t.size(); ++i) same = t[i].text == it->second[i].text && t[i].weight == it->second[i].weight;
        }
        chk(same, "learned terms at " + std::to_string(threads) + " threads equal the one-thread terms");
    }
    omp_set_num_threads(omp_get_num_procs());
    auto ov = std::make_shared<LearnOverlay>();
    Curator cur(ov);
    const auto t0 = std::chrono::steady_clock::now();
    cur.pass(s, kNow);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("fold of %zu cards: %.2f ms\n", s.cards.size(), ms);
    chk(ms < 250, "a 1000-card fold is quick");
}

// ---- the overlay reaching a catalogue ----
class FakeSource : public know::SkillSource {
public:
    explicit FakeSource(std::vector<know::Card> c) : cards_(std::move(c)) {}
    std::string name() const override { return "fake"; }
    std::vector<know::Card> list() override { return cards_; }
    std::optional<std::string> body(const know::Card&) override { return std::string("body"); }

private:
    std::vector<know::Card> cards_;
};

static std::vector<know::Card> deck() {
    std::vector<know::Card> v;
    const char* names[] = {"podman-ops", "kernel-tuning", "recall-router", "container-basics", "tape-cache", "vulkan-debug"};
    const char* gists[] = {"operate rootless containers and quadlet units", "sysctl and cache pressure", "route a question to memory", "what a container is", "kv cache of the tape", "debug vulkan shaders"};
    for (int i = 0; i < 6; ++i) {
        know::Card c;
        c.uri = skill(names[i]);
        c.title = names[i];
        c.gist = gists[i];
        v.push_back(c);
    }
    return v;
}

static know::Query q_of(std::initializer_list<const char*> ts) {
    know::Query q;
    for (const char* t : ts) q.terms.push_back({t, 1.0f});
    return q;
}

static void test_overlay_reaches_catalogue() {
    auto src = std::make_shared<FakeSource>(deck());
    know::SkillCatalogue plain({src});
    plain.refresh();
    const auto q = q_of({"ship", "rollout"});
    chk(plain.search(q, 3).empty(), "the vocabulary gap: nothing matches before learning");

    auto ov = std::make_shared<LearnOverlay>();
    know::SkillCatalogue cat({src});
    cat.set_overlay(ov);
    cat.refresh();
    const uint64_t g0 = cat.generation();
    chk(cat.search(q, 3).empty(), "an empty overlay changes nothing");

    Stats s;
    add_assoc(s, "podman-ops", "ship", 3, 2);
    add_assoc(s, "podman-ops", "rollout", 3, 2);
    Curator cur(ov);
    chk(cur.pass(s, kNow), "learning published");
    cat.refresh();
    chk(cat.generation() > g0, "the catalogue rebuilt on the moved version");
    const auto hits = cat.search(q, 3);
    chk(!hits.empty() && hits[0].card.title == "podman-ops", "the learned terms lift the card to the top of the gap query");
    cat.refresh();
    const uint64_t g1 = cat.generation();
    cat.refresh();
    chk(cat.generation() == g1, "no new version: no rebuild");

    // priors reach the score: a card with a better prior outranks an equal one
    Stats pr;
    give_efficacy(pr, "container-basics", 400, 0);
    for (int i = 0; i < 20; ++i) cur.pass(pr, kNow);
    cat.refresh();
    const auto h2 = cat.search(q_of({"container"}), 3);
    chk(h2.size() >= 2 && h2[0].card.title == "container-basics", "a proven card ranks first among near equals");
}

int main() {
    test_demand_rows();
    test_affinity_promotion();
    test_clamp_and_step();
    test_version_stable_on_noise();
    test_parallel_equals_serial_and_timing();
    test_overlay_reaches_catalogue();
    if (g_fail) std::printf("%d FAILED\n", g_fail);
    else std::printf("all learn overlay tests passed\n");
    return g_fail ? 1 : 0;
}
