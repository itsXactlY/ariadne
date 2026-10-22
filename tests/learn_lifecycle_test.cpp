// dd::learn::Lifecycle — every transition at its guard's edge, the file rewrite, the retired move, a year's walk.
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "dd/core/yaml.hpp"
#include "dd/know/skills.hpp"
#include "dd/learn/curator.hpp"
#include "dd/learn/lifecycle.hpp"
#include "dd/learn/service.hpp"

namespace fs = std::filesystem;
using namespace dd;
using namespace dd::learn;

static int g_fail = 0;
static void chk(bool ok, const std::string& label) {
    if (!ok) { ++g_fail; std::printf("FAIL %s\n", label.c_str()); }
}

constexpr int64_t D = 86400;
constexpr int64_t T0 = 1700000000;

static std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static fs::path make_skill(const fs::path& root, const std::string& cat, const std::string& name, const std::string& fm_extra, bool learned = true) {
    const fs::path d = root / cat / name;
    fs::create_directories(d);
    std::ofstream(d / "SKILL.md") << "---\nname: " << name << "\ndescription: when " << name << " matters\n" << (learned ? "origin: learned\n" : "") << fm_extra
                                  << "---\n\n# " << name << "\nthe body stays as it is\n";
    return d / "SKILL.md";
}

static std::string key(const std::string& name) { return "skill://" + name; }

static State file_state(const fs::path& md) {
    const auto fm = core::parse_frontmatter(slurp(md));
    const std::string s = fm.count("state") ? core::unquote(fm.at("state")) : "";
    for (State c : {State::Probation, State::Active, State::Dormant, State::Retired})
        if (s == state_name(c)) return c;
    return State::Draft;
}

struct Rig {
    fs::path root;
    Lifecycle life;
    std::vector<StateRec> recs;
    explicit Rig(const fs::path& r, LifecycleParams p = {}) : root(r), life(r, p) {}
    std::vector<Lifecycle::Change> step(const Stats& st, int64_t now) {
        return life.step(st, now, [this](const StateRec& r) { recs.push_back(r); });
    }
};

static fs::path fresh(const fs::path& base, const std::string& n) {
    fs::path p = base / n;
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

static void test_promote(const fs::path& base) {
    const fs::path root = fresh(base, "promote");
    const auto md = make_skill(root, "ops", "fix-a", "state: probation\nstate_since: " + std::to_string(T0) + "\n");
    Rig rig(root);
    Stats st;
    auto& c = st.cards[key("fix-a")];
    c.state = State::Probation;
    const int64_t now = T0 + 10 * D;
    c.helped.add(2.69, now, kHlEfficacy);
    chk(rig.step(st, now).empty() && file_state(md) == State::Probation, "promote: under 2.7 decayed wins is not enough");
    c.helped.add(0.02, now, kHlEfficacy);
    auto ch = rig.step(st, now);
    chk(ch.size() == 1 && ch[0].to == State::Active && file_state(md) == State::Active, "promote: 2.7 decayed wins (3, a few days apart) promote");
    chk(rig.recs.size() == 1 && rig.recs[0].from == State::Probation && rig.recs[0].to == State::Active, "promote: a StateRec was recorded");
    chk(slurp(md).find("the body stays as it is") != std::string::npos, "promote: the body is untouched");

    // a failure inside 30 days blocks; outside does not
    for (int64_t ago : {29 * D, 31 * D}) {
        const fs::path r2 = fresh(base, "promote_fail" + std::to_string(ago / D));
        const auto m2 = make_skill(r2, "ops", "fix-b", "state: probation\nstate_since: " + std::to_string(T0) + "\n");
        Rig rg(r2);
        Stats s2;
        auto& c2 = s2.cards[key("fix-b")];
        const int64_t n2 = T0 + 50 * D;
        c2.helped.add(3, n2, kHlEfficacy);
        c2.failed.add(1, n2 - ago, kHlEfficacy);
        rg.step(s2, n2);
        chk((file_state(m2) == State::Active) == (ago == 31 * D), "promote: one failure " + std::to_string(ago / D) + " days ago");
    }
}

static void test_retire_probation(const fs::path& base) {
    const fs::path root = fresh(base, "retire");
    const auto md = make_skill(root, "ops", "fix-c", "state: probation\nstate_since: " + std::to_string(T0) + "\n");
    Rig rig(root);
    Stats st;
    auto& c = st.cards[key("fix-c")];
    c.failed.add(1.9, T0 + D, kHlEfficacy);
    chk(rig.step(st, T0 + D).empty(), "retire: 1.9 failures keep it");
    c.failed.add(0.1, T0 + D, kHlEfficacy);
    auto ch = rig.step(st, T0 + D);
    chk(ch.size() == 1 && ch[0].to == State::Retired, "retire: 2 failures retire it");
    chk(!fs::exists(md) && fs::exists(Lifecycle::retired_dir(root, "ops", "fix-c") / "SKILL.md"), "retire: the folder moved to .retired, nothing deleted");
    chk(file_state(Lifecycle::retired_dir(root, "ops", "fix-c") / "SKILL.md") == State::Retired, "retire: the moved file says retired");

    // failures from before the probation began do not count
    const fs::path r2 = fresh(base, "retire_old");
    make_skill(r2, "ops", "fix-d", "state: probation\nstate_since: " + std::to_string(T0 + 10 * D) + "\n");
    Rig rg(r2);
    Stats s2;
    s2.cards[key("fix-d")].failed.add(3, T0, kHlEfficacy);
    chk(rg.step(s2, T0 + 11 * D).empty(), "retire: failures before the state began are not held against it");

    // no win in 60 days
    for (int64_t gap : {60 * D, 60 * D + 1}) {
        const fs::path r3 = fresh(base, "nowin" + std::to_string(gap));
        const auto m3 = make_skill(r3, "ops", "fix-e", "state: probation\nstate_since: " + std::to_string(T0) + "\n");
        Rig r(r3);
        Stats s3;
        s3.cards[key("fix-e")];
        r.step(s3, T0 + gap);
        chk(fs::exists(m3) == (gap == 60 * D), "retire: no win after " + std::to_string(gap) + " s");
    }
    // a win moves the clock
    const fs::path r4 = fresh(base, "nowin_reset");
    const auto m4 = make_skill(r4, "ops", "fix-f", "state: probation\nstate_since: " + std::to_string(T0) + "\n");
    Rig r(r4);
    Stats s4;
    s4.cards[key("fix-f")].helped.add(1, T0 + 30 * D, kHlEfficacy);
    r.step(s4, T0 + 89 * D);
    chk(fs::exists(m4), "retire: a win at day 30 keeps it past day 60");
    r.step(s4, T0 + 90 * D + 1);
    chk(!fs::exists(m4), "retire: ... but not past day 90");
}

static void test_dormant(const fs::path& base) {
    const fs::path root = fresh(base, "dormant");
    const auto md = make_skill(root, "ops", "fix-g", "state: active\nstate_since: " + std::to_string(T0) + "\n");
    Rig rig(root);
    Stats st;
    st.cards[key("fix-g")].last_used = T0 + 5 * D;
    rig.step(st, T0 + 5 * D + 120 * D);
    chk(file_state(md) == State::Active, "dormant: idle exactly 120 days stays active");
    auto ch = rig.step(st, T0 + 5 * D + 120 * D + 1);
    chk(ch.size() == 1 && ch[0].to == State::Dormant && file_state(md) == State::Dormant, "dormant: idle past 120 days");
    // used again
    st.cards[key("fix-g")].last_used = T0 + 200 * D;
    auto back = rig.step(st, T0 + 201 * D);
    chk(back.size() == 1 && back[0].to == State::Active, "dormant: use brings it back");
    // a year idle retires
    const fs::path r2 = fresh(base, "dormant_year");
    const auto m2 = make_skill(r2, "ops", "fix-h", "state: dormant\nstate_since: " + std::to_string(T0) + "\n");
    Rig rg(r2);
    Stats s2;
    s2.cards[key("fix-h")].last_used = T0 - D;
    rg.step(s2, T0 + 365 * D - D);   // idle exactly 365 d since last use
    chk(fs::exists(m2), "dormant: 365 days idle exactly stays");
    rg.step(s2, T0 + 365 * D + 1);
    chk(!fs::exists(m2) && fs::exists(Lifecycle::retired_dir(r2, "ops", "fix-h") / "SKILL.md"), "dormant: past a year idle retires, moved");
}

static void test_demote(const fs::path& base) {
    const fs::path root = fresh(base, "demote");
    const auto md = make_skill(root, "ops", "fix-i", "state: active\nstate_since: " + std::to_string(T0) + "\n");
    Rig rig(root);
    Stats st;
    auto& c = st.cards[key("fix-i")];
    const int64_t now = T0 + 20 * D;
    c.last_used = now;
    c.helped.add(1, now, kHlEfficacy);
    c.failed.add(3, now, kHlEfficacy);   // evidence 4: below the bar
    chk(rig.step(st, now).empty(), "demote: not enough evidence");
    c.failed.add(2, now, kHlEfficacy);   // 1 / 6
    auto ch = rig.step(st, now);
    chk(ch.size() == 1 && ch[0].to == State::Probation, "demote: poor efficacy with evidence demotes");
    chk(slurp(md).find("demoted: 1") != std::string::npos, "demote: the flag is in the file");

    // already demoted once: a file that says active + demoted does not go back
    const fs::path r2 = fresh(base, "demote_twice");
    const auto m2 = make_skill(r2, "ops", "fix-j", "state: active\nstate_since: " + std::to_string(T0) + "\ndemoted: 1\n");
    Rig rg(r2);
    Stats s2;
    auto& c2 = s2.cards[key("fix-j")];
    c2.last_used = now;
    c2.helped.add(1, now, kHlEfficacy);
    c2.failed.add(5, now, kHlEfficacy);
    rg.step(s2, now);
    chk(file_state(m2) == State::Active, "demote: only once");
}

static void test_files(const fs::path& base) {
    const fs::path root = fresh(base, "files");
    const auto human = make_skill(root, "ops", "human-skill", "state: probation\n", false);
    const std::string before = slurp(human);
    chk(!Lifecycle::rewrite_state(human, State::Retired, T0, false) && slurp(human) == before, "files: a skill without origin: learned is refused and untouched");
    chk(!Lifecycle::rewrite_state(root / "ops" / "nothing" / "SKILL.md", State::Active, T0, false), "files: a missing file is refused");

    Rig rig(root);
    Stats st;
    st.cards[key("human-skill")].failed.add(9, T0, kHlEfficacy);
    chk(rig.step(st, T0 + 400 * D).empty() && slurp(human) == before && rig.recs.empty(), "files: the lifecycle never looks at a human skill");

    const auto mine = make_skill(root, "ops", "mine", "state: probation\n");
    chk(Lifecycle::rewrite_state(mine, State::Active, 123, false), "files: a learned skill is rewritten");
    chk(!fs::exists(mine.parent_path() / ".SKILL.md.tmp"), "files: no temp file is left behind");
    const auto fm = core::parse_frontmatter(slurp(mine));
    chk(core::unquote(fm.at("state")) == "active" && core::unquote(fm.at("state_since")) == "123" && core::unquote(fm.at("origin")) == "learned", "files: state and since set, the rest kept");
    chk(slurp(mine).find("the body stays as it is") != std::string::npos, "files: body kept");
}

static void test_retired_unlisted(const fs::path& base) {
    const fs::path root = fresh(base, "unlisted");
    make_skill(root, "ops", "keep-me", "state: active\nstate_since: " + std::to_string(T0) + "\n");
    make_skill(root, "ops", "lose-me", "state: probation\nstate_since: " + std::to_string(T0) + "\n");
    know::DiskSkills disk({root});
    chk(disk.list().size() == 2, "unlisted: both are listed first");
    Rig rig(root);
    Stats st;
    st.cards[key("keep-me")].last_used = T0 + 70 * D;
    rig.step(st, T0 + 70 * D);
    const auto cards = disk.list();
    bool lost = false, kept = false;
    for (const auto& c : cards) {
        lost |= c.title == "lose-me";
        kept |= c.title == "keep-me";
    }
    chk(kept && !lost, "unlisted: a retired skill is no longer listed by DiskSkills");
}

static void test_reconcile(const fs::path& base) {
    const fs::path root = fresh(base, "reconcile");
    make_skill(root, "ops", "fix-k", "state: probation\nstate_since: " + std::to_string(T0) + "\n");
    Rig rig(root);
    Stats st;   // the journal never heard of it
    rig.step(st, T0 + D);
    chk(rig.recs.size() == 1 && rig.recs[0].why == "reconcile" && rig.recs[0].to == State::Probation, "reconcile: the file leads, the journal is told");
    st.apply(Record{rig.recs[0]});
    rig.recs.clear();
    rig.step(st, T0 + D);
    chk(rig.recs.empty(), "reconcile: told once");
}

static void test_prior_uncapped() {
    Stats st;
    const int64_t now = T0 + 100 * D;
    for (const char* n : {"p", "a"}) {
        auto& c = st.cards[key(n)];
        c.helped.add(30, now, kHlEfficacy);
        c.state = n[0] == 'p' ? State::Probation : State::Active;
    }
    auto ov = std::make_shared<LearnOverlay>();
    Curator cur(ov);
    for (int i = 0; i < 12; ++i) cur.pass(st, now);
    know::Uri up, ua;
    up.id = "p";
    ua.id = "a";
    chk(ov->prior(up) <= 0.7 + 1e-9, "prior: a card on probation stays under its cap (" + std::to_string(ov->prior(up)) + ")");
    chk(ov->prior(ua) > 0.7 + 0.05, "prior: a promoted card is uncapped (" + std::to_string(ov->prior(ua)) + ")");
}

// a year, day by day: wins in the first days, use for a while, then silence
static void test_year(const fs::path& base) {
    const fs::path root = fresh(base, "year");
    const auto md = make_skill(root, "ops", "fix-year", "state: probation\nstate_since: " + std::to_string(T0) + "\n");
    Rig rig(root);
    Stats st;
    auto& c = st.cards[key("fix-year")];
    std::vector<std::pair<int, State>> seq;
    State last = State::Probation;
    for (int day = 1; day <= 800; ++day) {
        const int64_t now = T0 + day * D;
        if (day == 2 || day == 3 || day == 4) {
            c.helped.add(1, now, kHlEfficacy);
            c.last_used = now;
        }
        if (day > 4 && day <= 100 && day % 10 == 0) c.last_used = now;   // used now and then
        const auto ch = rig.step(st, now);
        for (const auto& x : ch) {
            seq.emplace_back(day, x.to);
            for (const auto& r : rig.recs) st.apply(Record{r});
            rig.recs.clear();
            last = x.to;
        }
        for (const auto& r : rig.recs) st.apply(Record{r});   // reconcile
        rig.recs.clear();
        if (last == State::Retired) break;
    }
    chk(seq.size() == 3 && seq[0].second == State::Active && seq[1].second == State::Dormant && seq[2].second == State::Retired, "year: probation -> active -> dormant -> retired, in that order (" + std::to_string(seq.size()) + " steps)");
    if (seq.size() == 3) {
        chk(seq[0].first >= 4 && seq[0].first <= 5, "year: promoted when the third win landed (day " + std::to_string(seq[0].first) + ")");
        chk(seq[1].first == 100 + 121, "year: dormant the day after 120 idle days (day " + std::to_string(seq[1].first) + ")");
        chk(seq[2].first > seq[1].first + 360, "year: retired after a year of dormancy (day " + std::to_string(seq[2].first) + ")");
    }
    chk(!fs::exists(md) && fs::exists(Lifecycle::retired_dir(root, "ops", "fix-year") / "SKILL.md"), "year: the skill ends in .retired, not deleted");
}

// the service: a skill the journal drafted ages out with the injected clock
static void test_service_clock(const fs::path& base) {
    const fs::path root = fresh(base, "svc");
    std::atomic<int64_t> clk{T0};
    ServiceOptions o;
    o.dir = root / "learn";
    o.skills_root = root / "skills";
    o.home_dir = "/home/nobody";
    o.project_dir = root.string();
    o.curate_every = std::chrono::seconds(100000000);
    o.clock = [&clk] { return clk.load(); };
    auto svc = LearnService::start(o);
    chk(svc != nullptr, "service: starts with an injected clock");
    if (!svc) return;
    const auto md = make_skill(o.skills_root, "ops", "svc-fix", "state: probation\nstate_since: " + std::to_string(T0) + "\n");
    know::Event e;
    e.type = know::Event::Type::Rebase;
    svc->sink()->emit(std::move(e));
    svc->wait_idle(std::chrono::seconds(2));
    svc->curate_now();
    for (int i = 0; i < 400 && svc->status().passes < 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    chk(fs::exists(md) && svc->status().transitions == 0, "service: young skill stays");
    clk = T0 + 61 * D;
    const uint64_t p = svc->status().passes;
    svc->curate_now();
    for (int i = 0; i < 400 && svc->status().passes <= p; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    chk(!fs::exists(md) && svc->status().transitions == 1, "service: 61 days without a win retires it");
    svc->stop();

    auto again = LearnService::start(o);
    chk(again && again->status().ledger.recovered > 0, "service: the retirement is in the journal after a restart");
}

int main() {
    const fs::path base = fs::path("build") / ("learn_lifecycle_tmp_" + std::to_string(::getpid()));
    fs::remove_all(base);
    fs::create_directories(base);

    test_promote(base);
    test_retire_probation(base);
    test_dormant(base);
    test_demote(base);
    test_files(base);
    test_retired_unlisted(base);
    test_reconcile(base);
    test_prior_uncapped();
    test_year(base);
    test_service_clock(base);

    fs::remove_all(base);
    if (g_fail) std::printf("%d FAILED\n", g_fail);
    else std::printf("learn lifecycle: all passed\n");
    return g_fail ? 1 : 0;
}
