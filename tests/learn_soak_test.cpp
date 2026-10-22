// dd::learn soak: a time-compressed half year with injected faults, against fixed caps.
//   1. crash campaign   fork + SIGKILL mid-append and mid-compaction; every recovery is a prefix of the truth
//   2. 180-day sim      600 skills, hidden ground truth, clock jumps, a pod outage, secrets in errors, a human skill
//   3. downgrade        newer-schema frames are skipped and counted, a newer snapshot refuses to open and is left alone
//   4. service burst    the real LearnService: a flood of solved problems in one day stays under the daily cap
// Env: DD_SOAK_DAYS (180) DD_SOAK_TURNS (300 per day) DD_SOAK_CRASHES (40) DD_SOAK_SEED. Files live under build/.
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "dd/learn/author.hpp"
#include "dd/learn/codec.hpp"
#include "dd/learn/compactor.hpp"
#include "dd/learn/curator.hpp"
#include "dd/learn/detector.hpp"
#include "dd/learn/episode.hpp"
#include "dd/learn/journal.hpp"
#include "dd/learn/ledger.hpp"
#include "dd/learn/observer.hpp"
#include "dd/learn/overlay.hpp"
#include "dd/learn/reviewer.hpp"
#include "dd/learn/service.hpp"
#include "dd/learn/skill_sink.hpp"

namespace fs = std::filesystem;
using namespace dd;
using namespace dd::learn;
using Clock = std::chrono::steady_clock;
using T = know::Event::Type;

static int g_fail = 0;
static void chk(bool ok, const std::string& label) {
    if (!ok) {
        ++g_fail;
        std::printf("FAIL %s\n", label.c_str());
    }
}

static long env_long(const char* k, long d) {
    const char* e = std::getenv(k);
    return e ? std::atol(e) : d;
}

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd() {
    g_rng ^= g_rng << 13, g_rng ^= g_rng >> 7, g_rng ^= g_rng << 17;
    return g_rng;
}
static double unit() { return static_cast<double>(rnd() >> 11) / 9007199254740992.0; }

static fs::path fresh(const std::string& name) {
    const fs::path p = fs::path("build") / "learn_soak_tmp" / name;
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

static double rss_mib() {
    std::ifstream f("/proc/self/status");
    std::string l;
    while (std::getline(f, l))
        if (l.rfind("VmRSS:", 0) == 0) return std::atof(l.c_str() + 6) / 1024.0;
    return 0;
}

static uint64_t dir_bytes(const fs::path& d) {
    uint64_t n = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(d, ec))
        if (e.is_regular_file(ec)) n += e.file_size(ec);
    return n;
}

static std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream o;
    o << f.rdbuf();
    return o.str();
}

static double since_ms(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

static know::Uri skill(int i) {
    know::Uri u;
    u.kind = know::Kind::Skill;
    u.id = "s" + std::to_string(i);
    return u;
}

constexpr int64_t kT0 = 1760000000;   // 2025-10-09

static know::Event mk(T t, uint32_t turn, uint32_t round, int64_t sec) {
    know::Event e;
    e.type = t;
    e.turn = turn;
    e.round = round;
    e.ts_ns = static_cast<uint64_t>(sec) * 1000000000ull;
    return e;
}

// ---------------------------------------------------------------- 1. crash campaign
// The i-th record of a fixed, endless script: a recovery must hold exactly its first n.
static Record gen(uint64_t i) {
    const int64_t ts = kT0 + static_cast<int64_t>(i) * 3;
    const know::Uri u = skill(static_cast<int>(i % 600));
    if (i % 997 == 996) return TombstoneRec{skill(static_cast<int>((i * 13) % 600)), ts};
    switch (i % 7) {
        case 0: {
            OutcomeRec o;
            o.uri = u;
            o.outcome = static_cast<Outcome>(i % 5);
            o.trigger = "turn";
            o.terms = {"t" + std::to_string(i % 50), "u" + std::to_string(i % 13)};
            o.episode = static_cast<uint32_t>(i / 50);
            o.ts = ts;
            return o;
        }
        case 1:
        case 2:
        case 3:
        case 4: {
            know::Event e = mk(i % 7 == 1 ? T::Offered : i % 7 == 2 ? T::OpenedByModel : i % 7 == 3 ? T::OpenedByRouter : T::Shadow, static_cast<uint32_t>(i / 9), static_cast<uint32_t>(i % 5), ts);
            e.uri = u;
            e.reason = "turn";
            e.matched = 3;
            e.mass = 8;
            e.score = 0.4f;
            return EventRec{e};
        }
        case 5: return StateRec{u, State::Draft, State::Probation, "gen", ts};
        default: {
            DraftRec d;
            d.slug = "d" + std::to_string(i % 40);
            d.category = "learned";
            d.digest = "x";
            d.accepted = true;
            d.ts = ts;
            return d;
        }
    }
}

static LedgerOptions crash_opts() {
    LedgerOptions o;
    o.journal.segment_bytes = 32u << 10;
    o.journal.sync_records = 64;
    o.compact_after = 2;
    o.publish_every = 512;
    return o;
}

static void test_crashes(int rounds) {
    const fs::path dir = fresh("crash");
    Stats oracle;
    uint64_t m = 0, hook_kills = 0, timed_kills = 0, torn = 0, snaps = 0, max_open_ms = 0;
    double open_ms_sum = 0;
    for (int r = 0; r < rounds; ++r) {
        const bool hook = (r % 2) == 0;
        const int steps = 1 + static_cast<int>(rnd() % 30);
        const useconds_t wait_us = 3000 + static_cast<useconds_t>(rnd() % 50000);
        const pid_t pid = ::fork();
        if (pid == 0) {
            LedgerOptions o = crash_opts();
            if (hook) {
                auto left = std::make_shared<int>(steps);
                o.compact_hook = [left](const char*) {
                    if (--*left == 0) ::raise(SIGKILL);
                    return true;
                };
            }
            auto l = Ledger::open(dir, o);
            if (!l) ::_exit(3);
            const uint64_t start = l->live().records;
            for (uint64_t i = start; i < start + 400000; ++i) {
                l->record(gen(i));
                if ((i & 255) == 0) l->tick();
            }
            ::_exit(0);
        }
        if (!hook) {
            ::usleep(wait_us);
            ::kill(pid, SIGKILL);
        }
        int st = 0;
        ::waitpid(pid, &st, 0);
        if (WIFSIGNALED(st)) (hook ? hook_kills : timed_kills)++;
        else chk(false, "crash round " + std::to_string(r) + ": the child was not killed (exit " + std::to_string(WEXITSTATUS(st)) + ")");

        const auto t0 = Clock::now();
        std::string err;
        auto l = Ledger::open(dir, crash_opts(), &err);
        const double ms = since_ms(t0);
        open_ms_sum += ms;
        max_open_ms = std::max<uint64_t>(max_open_ms, static_cast<uint64_t>(ms));
        chk(l != nullptr, "crash round " + std::to_string(r) + ": recovery opens: " + err);
        if (!l) return;
        const uint64_t n = l->live().records;
        chk(n >= m, "crash round " + std::to_string(r) + ": recovery never goes back (" + std::to_string(n) + " < " + std::to_string(m) + ")");
        for (; m < n; ++m) oracle.apply(canonical(gen(m)));
        chk(n < m || encode_stats(l->live()) == encode_stats(oracle), "crash round " + std::to_string(r) + ": recovered state == fold of the first " + std::to_string(n) + " records");
        chk(!fs::exists(dir / "snap.tmp"), "crash round " + std::to_string(r) + ": no temp snapshot left");
        const auto h = l->health();
        torn += h.torn_bytes;
        snaps += h.compactions;
        // a recovered ledger takes on where the prefix ends
        l->record(gen(n));
        oracle.apply(canonical(gen(n)));
        ++m;
        chk(encode_stats(l->live()) == encode_stats(oracle), "crash round " + std::to_string(r) + ": the next record folds onto the prefix");
    }
    std::printf("crashes: %d rounds (%llu killed mid-compaction, %llu killed mid-append), %llu records survived, open avg %.1f ms max %llu ms, torn bytes trimmed %llu\n", rounds, (unsigned long long)hook_kills, (unsigned long long)timed_kills, (unsigned long long)m, open_ms_sum / std::max(1, rounds), (unsigned long long)max_open_ms, (unsigned long long)torn);
    (void)snaps;
}

// ---------------------------------------------------------------- 2. the half year
constexpr int kSkills = 600, kHub = 20;

static bool good(int s) { return s % 5 == 0; }
static int hub_card(int k) { return 5 * k; }   // twenty equally good cards
static bool is_hub(int s) { return s % 5 == 0 && s / 5 < kHub; }

struct Problem {
    std::string error, binary, cmd;
};

static Problem problem(int p) {
    static const char* bins[] = {"systemctl", "podman", "make", "git", "cargo", "rsync", "ssh", "npm"};
    Problem x;
    x.binary = bins[p % 8];
    const std::string w = "unit" + std::to_string(p) + "x" + std::to_string(p * 7919 % 1000);
    x.error = "error: " + w + " refused handshake with backend " + std::to_string(p % 13) + " code E" + std::to_string(1000 + p);
    if (p % 20 == 3) x.error += " Authorization: Bearer abcdef1234567890abcdef1234567890";   // a secret in an error line
    x.cmd = x.binary + " run " + w;
    return x;
}

struct Sim {
    fs::path dir, skills_root;
    LedgerOptions lopt;
    std::unique_ptr<Ledger> ledger;
    std::shared_ptr<LearnOverlay> overlay = std::make_shared<LearnOverlay>();
    std::unique_ptr<Curator> curator;
    std::unique_ptr<Observer> obs;
    Stats shadow;                                   // an in-memory fold of every record handed to the ledger
    double rec_ms = 0;
    uint64_t recs = 0;

    void open() {
        std::string err;
        ledger = Ledger::open(dir, lopt, &err);
        chk(ledger != nullptr, "sim: ledger opens: " + err);
    }
    void rec(Record r) {
        const auto t0 = Clock::now();
        const bool ok = ledger->record(r);
        rec_ms += since_ms(t0);
        ++recs;
        if (!ok) chk(false, "sim: a record was refused");
        shadow.apply(canonical(r));
    }
};

static std::vector<double> priors(const LearnOverlay& o) {
    std::vector<double> v(kSkills);
    for (int i = 0; i < kSkills; ++i) v[i] = o.prior(skill(i));
    return v;
}

struct Result {
    std::vector<std::string> drafts;   // names written
    uint64_t rejected = 0;
};

static int64_t day_offset(int d) {   // clock trouble: a step back of five days, and a jump forward of forty
    int64_t o = 0;
    if (d >= 60 && d < 70) o -= 5 * 86400;
    if (d >= 100 && d < 103) o += 40 * 86400;
    return o;
}

static void test_downgrade(Sim& sim, const fs::path& scratch) {
    // a downgraded binary meets frames of a newer codec in the journal
    const std::string before = encode_stats(sim.shadow);
    sim.ledger->sync();
    const uint64_t last = sim.ledger->health().last_seq;
    sim.ledger.reset();
    const auto segs = Journal::segments(sim.dir);
    chk(!segs.empty(), "downgrade: a journal exists");
    {
        std::string future;
        for (uint64_t k = 1; k <= 5; ++k) frame_encode(future, kCodecVersion + 1, last + k, "bytes from a newer schema");
        std::ofstream f(Journal::segment_path(sim.dir, segs.back()), std::ios::binary | std::ios::app);
        f << future;
    }
    sim.open();
    if (!sim.ledger) return;
    chk(sim.ledger->health().replay_newer == 5, "downgrade: five newer frames skipped and counted");
    chk(sim.ledger->health().last_seq == last + 5, "downgrade: the sequence goes past them");
    chk(encode_stats(sim.ledger->live()) == before, "downgrade: the state is exactly what it was");
    sim.rec(gen(1));
    chk(sim.ledger->health().last_seq == last + 6, "downgrade: appends go on after the skipped frames");

    // a newer snapshot: open refuses and touches nothing
    sim.ledger->compact_now();
    sim.ledger->sync();
    const fs::path copy = scratch / "newer_snapshot";
    std::error_code ec;
    fs::remove_all(copy, ec);
    fs::copy(sim.dir, copy, fs::copy_options::recursive, ec);
    fs::path snap;
    for (const auto& e : fs::directory_iterator(copy))
        if (e.path().extension() == ".dat") snap = e.path();
    chk(!snap.empty(), "downgrade: a snapshot exists to age");
    if (snap.empty()) return;
    std::string b = slurp(snap);
    const uint32_t newer = kStatsVersion + 1;
    std::memcpy(&b[4], &newer, 4);
    std::ofstream(snap, std::ios::binary | std::ios::trunc) << b;
    std::map<std::string, std::string> files;
    for (const auto& e : fs::directory_iterator(copy)) files[e.path().filename().string()] = slurp(e.path());
    std::string err;
    chk(!Ledger::open(copy, sim.lopt, &err) && err.find("newer") != std::string::npos, "downgrade: a newer snapshot refuses to open (" + err + ")");
    bool same = true;
    size_t seen = 0;
    for (const auto& e : fs::directory_iterator(copy)) {
        ++seen;
        same = same && files.count(e.path().filename().string()) && files[e.path().filename().string()] == slurp(e.path());
    }
    chk(same && seen == files.size(), "downgrade: ... and leaves every file as it found it");
    fs::remove_all(copy, ec);
}

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool kSanitized = true;   // shadow memory and quarantine make RSS meaningless as a cap
#else
constexpr bool kSanitized = false;
#endif

static void test_sim(int days, int turns_per_day) {
    const bool converged = days >= 120 && turns_per_day >= 200;   // a short run has not learned enough to judge
    Sim sim;
    sim.dir = fresh("sim") / "learn";
    sim.skills_root = fresh("sim_skills");
    sim.lopt.journal.segment_bytes = 256u << 10;
    sim.lopt.journal.sync_records = 256;
    sim.lopt.compact_after = 2;
    sim.open();
    if (!sim.ledger) return;
    sim.curator = std::make_unique<Curator>(sim.overlay);
    Sim* sp = &sim;
    sim.obs = std::make_unique<Observer>([sp](OutcomeRec&& o) { sp->rec(Record{std::move(o)}); });

    // a human skill that a draft will want the name of
    EpisodeRecorder recorder;
    std::vector<std::unique_ptr<Detector>> dets;
    dets.push_back(std::make_unique<FixedAfterFailure>());
    dets.push_back(std::make_unique<LongSuccess>());
    TemplateAuthor author("/home/nobody");
    Reviewer::Options ro;
    ro.home_dir = "/home/nobody";
    ro.project_dir = "/home/nobody/work";
    Reviewer reviewer([](const know::Query&) { return std::vector<know::Hit>(); }, ro);
    SkillSink sink(sim.skills_root);
    const int human_problem = 7;
    std::string human_path_s, human_text;
    {
        Detection d;
        d.kind = "fixed";
        const Problem pr = problem(human_problem);
        for (int k = 0; k < 6; ++k) {
            Step s;
            s.turn = 1;
            s.round = static_cast<uint32_t>(k + 1);
            s.call.tool = "run_terminal";
            s.call.args = pr.cmd;
            s.call.binaries = {pr.binary};
            s.call.failed = k < 2;
            s.call.error = k < 2 ? pr.error : "";
            d.slice.steps.push_back(s);
        }
        d.signature = pr.error;
        if (auto dr = author.author(d)) {
            const fs::path p = sink.path_of(*dr);
            fs::create_directories(p.parent_path());
            human_text = "---\nname: " + dr->name + "\ndescription: written by a human\n---\nDo not touch this.\n";
            std::ofstream(p) << human_text;
            human_path_s = p.string();
        }
    }
    chk(!human_path_s.empty(), "sim: the human skill's name is known");

    // hidden truth and the sim's view of the catalogue
    std::vector<double> vis(kSkills, 1.0);   // the priors the catalogue last refreshed with
    std::vector<uint64_t> hub_offers(kHub, 0);
    uint64_t hub_total = 0;
    std::vector<std::string> drafted;
    uint64_t accepted = 0, rejected = 0, max_per_day = 0, secret_hits = 0;
    const double rss0 = rss_mib();
    double rss_peak = 0, rss_mid_peak = 0, rss_late_peak = 0;
    uint64_t disk_peak = 0;
    uint32_t turn = 0;
    double max_step = 0, min_prior = 1, max_prior = 1;
    bool outage_caught_up = true;
    const int cap_per_day = 3, outage_a = 90, outage_b = 100, downgrade_day = 120;
    uint64_t overlay_v_at_outage = 0;
    const auto wall0 = Clock::now();
    std::printf("day   rss_MiB  disk_KiB  records  segs  skills  overlay_v\n");

    for (int d = 0; d < days; ++d) {
        const int64_t base = kT0 + static_cast<int64_t>(d) * 86400 + day_offset(d);
        const bool outage = d >= outage_a && d < outage_b;
        // the catalogue refreshes each morning unless the pod is away (nothing new is seen for ten days)
        if (!outage) {
            vis = priors(*sim.overlay);
            if (d == outage_b) outage_caught_up = sim.overlay->version() >= overlay_v_at_outage;
        } else if (d == outage_a) {
            overlay_v_at_outage = sim.overlay->version();
        }

        for (int t = 0; t < turns_per_day; ++t) {
            ++turn;
            const int64_t now = base + static_cast<int64_t>(t) * 86400 / std::max(1, turns_per_day);
            sim.obs->feed(mk(T::TurnStart, turn, 0, now));
            const bool hub = (rnd() & 1) != 0;
            std::vector<int> cand;
            if (hub) {
                for (int k = 0; k < kHub; ++k) cand.push_back(hub_card(k));
            } else {
                while (cand.size() < 3) {
                    const int c = static_cast<int>(rnd() % kSkills);
                    if (is_hub(c) || std::find(cand.begin(), cand.end(), c) != cand.end()) continue;
                    cand.push_back(c);
                }
            }
            std::vector<std::pair<double, int>> ranked;
            for (int c : cand) ranked.push_back({(1.0 + (unit() - 0.5) * (hub ? 0.2 : 0.6)) * vis[c], c});
            std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            const int top = ranked[0].second;
            if (hub) {
                ++hub_offers[static_cast<size_t>(top / 5)];
                ++hub_total;
            }
            know::Event off = mk(T::Offered, turn, 0, now);
            off.uri = skill(top);
            const bool by_failure = (rnd() & 1) != 0;   // half the offers answer an error, so a repeat of it is the verdict
            off.reason = by_failure ? "failure" : "turn";
            off.key = by_failure ? "boom" : "";
            off.matched = 3;
            off.mass = 9;
            off.score = 0.4f;
            off.terms = {"c" + std::to_string(top) + "a", "c" + std::to_string(top) + "b", "q" + std::to_string(rnd() % 40)};
            sim.rec(EventRec{off});
            sim.obs->feed(off);
            for (size_t k = 1; k < std::min<size_t>(3, ranked.size()); ++k) {   // the runners-up were near misses
                know::Event sh = mk(T::Shadow, turn, 0, now);
                sh.uri = skill(ranked[k].second);
                sh.reason = "turn";
                sh.matched = 2;
                sh.mass = 5;
                sim.rec(EventRec{sh});
                sim.obs->feed(sh);
            }
            const bool opened = unit() < (good(top) ? 0.7 : 0.35);
            if (opened) {
                know::Event op = mk(T::OpenedByModel, turn, 1, now);
                op.uri = skill(top);
                sim.rec(EventRec{op});
                sim.obs->feed(op);
            }
            const bool works = opened && unit() < (good(top) ? 0.9 : 0.25);
            for (uint32_t r = 2; r <= 4; ++r) {
                know::Event c = mk(T::Calls, turn, r, now);
                c.call.tool = "run_terminal";
                c.call.failed = opened && !works && r >= 3;
                c.call.error = c.call.failed ? "boom" : "";
                sim.obs->feed(c);
            }
            sim.obs->feed(mk(T::TurnEnd, turn, 4, now));
        }

        // solved problems of the day, through the authoring chain under the daily cap
        const int episodes = static_cast<int>(rnd() % 7);
        int written_today = 0;
        for (int k = 0; k < episodes; ++k) {
            const int p = static_cast<int>(rnd() % 400);
            const Problem pr = problem(p);
            ++turn;
            const int64_t now = base + 43200 + k;
            recorder.feed(mk(T::TurnStart, turn, 0, now));
            for (uint32_t r = 1; r <= 6; ++r) {
                know::Event c = mk(T::Calls, turn, r, now);
                c.call.tool = "run_terminal";
                c.call.args = pr.cmd;
                c.call.binaries = {pr.binary};
                c.call.failed = r <= 2;
                c.call.error = r <= 2 ? pr.error : "";
                c.call.digest = pr.cmd + std::to_string(r);
                recorder.feed(c);
            }
            recorder.feed(mk(T::TurnEnd, turn, 6, now));
            const auto det = best_detection(dets, recorder.window());
            if (!det) continue;
            recorder.drop_through(det->end_turn, det->end_round);
            if (written_today >= cap_per_day) continue;
            const auto dr = author.author(*det);
            if (!dr) continue;
            const Verdict v = reviewer.review(*dr);
            if (!v.ok) {
                sim.rec(Record{sink.reject(*dr, v)});
                ++rejected;
                continue;
            }
            DraftRec rec = sink.write(*dr, v);
            sim.rec(Record{rec});
            if (rec.accepted) {
                know::Uri u;
                u.kind = know::Kind::Skill;
                u.id = dr->name;
                sim.rec(Record{StateRec{u, State::Draft, State::Probation, "template", now}});
                drafted.push_back(dr->name);
                ++written_today;
                ++accepted;
            } else {
                ++rejected;
            }
        }
        max_per_day = std::max<uint64_t>(max_per_day, static_cast<uint64_t>(written_today));

        // the nightly curator pass, and its limits
        sim.ledger->tick();
        const std::vector<double> before = priors(*sim.overlay);
        sim.curator->pass(sim.ledger->live(), base + 86399);
        const std::vector<double> after = priors(*sim.overlay);
        for (int i = 0; i < kSkills; ++i) {
            min_prior = std::min(min_prior, after[i]);
            max_prior = std::max(max_prior, after[i]);
            if (before[i] > 0) max_step = std::max(max_step, std::fabs(after[i] / before[i] - 1.0));
        }

        if (d == downgrade_day) test_downgrade(sim, sim.dir.parent_path());
        if (!sim.ledger) return;

        const double rss = rss_mib();
        const uint64_t disk = dir_bytes(sim.dir);
        rss_peak = std::max(rss_peak, rss);
        disk_peak = std::max(disk_peak, disk);
        if (d >= days / 6 && d < days / 2) rss_mid_peak = std::max(rss_mid_peak, rss);
        if (d >= days * 2 / 3) rss_late_peak = std::max(rss_late_peak, rss);
        if (d % std::max(1, days / 18) == 0 || d == days - 1)
            std::printf("%3d  %8.1f  %8llu  %7llu  %4zu  %6zu  %9llu\n", d, rss, (unsigned long long)(disk / 1024), (unsigned long long)sim.shadow.records, Journal::segments(sim.dir).size(), drafted.size(), (unsigned long long)sim.overlay->version());
    }
    const double wall_s = since_ms(wall0) / 1000.0;

    // ---- end-of-run assertions
    chk(min_prior >= 0.5 - 1e-9 && max_prior <= 1.5 + 1e-9, "sim: priors stay in [0.5, 1.5] (saw " + std::to_string(min_prior) + " .. " + std::to_string(max_prior) + ")");
    chk(max_step <= 0.05 + 1e-6, "sim: a prior moves at most 5% per pass (max " + std::to_string(max_step) + ")");
    if (!kSanitized) chk(rss_peak - rss0 <= 160.0, "sim: peak RSS growth under 160 MiB (" + std::to_string(rss_peak - rss0) + ")");
    if (!kSanitized) chk(rss_late_peak <= rss_mid_peak * 1.25 + 16.0, "sim: RSS is flat over the last third (mid peak " + std::to_string(rss_mid_peak) + ", late peak " + std::to_string(rss_late_peak) + ")");
    chk(disk_peak <= 32ull << 20, "sim: journal + snapshot bytes stay under 32 MiB (peak " + std::to_string(disk_peak >> 10) + " KiB)");
    chk(max_per_day <= static_cast<uint64_t>(cap_per_day), "sim: skills written per day within the cap");
    chk(outage_caught_up, "sim: the overlay is current again after the pod outage");

    // equally good cards share the offers
    const double uniform = 1.0 / kHub;
    const int late_from = std::max(0, days - 30);
    (void)late_from;
    uint64_t top1 = 0;
    for (uint64_t c : hub_offers) top1 = std::max(top1, c);
    const double share = hub_total ? static_cast<double>(top1) / static_cast<double>(hub_total) : 0;
    {
        const std::vector<double> pr = priors(*sim.overlay);
        std::printf("hub:");
        for (int k = 0; k < kHub; ++k) std::printf(" %llu@%.2f", (unsigned long long)hub_offers[static_cast<size_t>(k)], pr[static_cast<size_t>(hub_card(k))]);
        std::printf("\n");
    }
    if (converged) chk(share < 2 * uniform, "sim: no hub collapse, top-1 share of twenty equal cards " + std::to_string(share) + " (uniform " + std::to_string(uniform) + ")");

    // the learner told good cards from bad ones
    {
        const std::vector<double> pr = priors(*sim.overlay);
        double g = 0, b = 0;
        int ng = 0, nb = 0;
        for (int i = 0; i < kSkills; ++i) {
            if (is_hub(i)) continue;
            const auto it = sim.shadow.cards.find(skill(i).str());
            if (it == sim.shadow.cards.end() || it->second.helped.v + it->second.failed.v < 8) continue;
            (good(i) ? g : b) += pr[static_cast<size_t>(i)];
            ++(good(i) ? ng : nb);
        }
        std::printf("sim: mean prior of well-sampled cards: good %.3f (%d), bad %.3f (%d)\n", ng ? g / ng : 0, ng, nb ? b / nb : 0, nb);
        if (converged) chk(ng > 0 && nb > 0 && g / ng > b / nb + 0.1, "sim: good cards end with clearly higher priors than bad ones");
    }

    // drafts: on disk, learned, never a secret, never over the human skill
    chk(accepted > 0, "sim: skills were drafted (" + std::to_string(accepted) + ")");
    size_t files = 0;
    for (const auto& e : fs::recursive_directory_iterator(sim.skills_root))
        if (e.path().filename() == "SKILL.md") {
            ++files;
            const std::string t = slurp(e.path());
            if (t.find("abcdef1234567890") != std::string::npos) ++secret_hits;
            if (e.path().string() != human_path_s) chk(t.find("origin: learned") != std::string::npos, "sim: a written skill is marked learned: " + e.path().string());
        }
    chk(secret_hits == 0, "sim: no secret reached a skill file");
    chk(slurp(human_path_s) == human_text, "sim: the human skill is byte for byte what it was");
    std::sort(drafted.begin(), drafted.end());
    drafted.erase(std::unique(drafted.begin(), drafted.end()), drafted.end());
    chk(files <= drafted.size() + 1, "sim: the same problem again updates a file, it does not add one (" + std::to_string(files) + " files, " + std::to_string(drafted.size()) + " names)");
    chk(rejected > 0, "sim: some drafts were refused (the planted secrets)");
    int bad_state = 0;
    for (const auto& n : drafted) {
        know::Uri u;
        u.kind = know::Kind::Skill;
        u.id = n;
        const auto it = sim.ledger->live().cards.find(u.str());
        if (it == sim.ledger->live().cards.end() || (it->second.state != State::Probation && it->second.state != State::Active)) ++bad_state;
    }
    chk(bad_state == 0, "sim: every drafted skill ends Probation or Active (" + std::to_string(bad_state) + " do not)");

    // replay of everything on disk == the live state == the in-memory fold of every record
    sim.obs->flush();
    sim.ledger->sync();
    const std::string live = encode_stats(sim.ledger->live());
    chk(live == encode_stats(sim.shadow), "sim: the ledger's state == the fold of every record handed to it");
    const uint64_t compactions = sim.ledger->health().compactions;
    sim.ledger.reset();
    const auto t0 = Clock::now();
    std::string err;
    auto again = Ledger::open(sim.dir, sim.lopt, &err);
    const double open_ms = since_ms(t0);
    chk(again != nullptr, "sim: reopen: " + err);
    if (again) chk(encode_stats(again->live()) == live, "sim: replay of the journal == the live state, bit for bit");
    const double rate = sim.rec_ms > 0 ? static_cast<double>(sim.recs) / (sim.rec_ms / 1000.0) : 0;
    std::printf("sim: %d days, %llu records (%.0f/s through the ledger, %.1f s wall), %zu cards, %zu assoc, %llu compactions\n", days, (unsigned long long)sim.recs, rate, wall_s, sim.shadow.cards.size(), sim.shadow.assoc.size(), (unsigned long long)compactions);
    std::printf("sim: peak RSS %.1f MiB (start %.1f), peak disk %llu KiB, final reopen %.1f ms, prior range %.3f..%.3f, max step %.4f\n", rss_peak, rss0, (unsigned long long)(disk_peak >> 10), open_ms, min_prior, max_prior, max_step);
    std::printf("sim: drafted %llu (%zu names, %zu files), refused %llu, max %llu/day, hub top-1 share %.3f\n", (unsigned long long)accepted, drafted.size(), files, (unsigned long long)rejected, (unsigned long long)max_per_day, share);
}

// ---------------------------------------------------------------- 4. the real service under a flood
static void test_service_burst() {
    const fs::path root = fresh("burst");
    ServiceOptions o;
    o.dir = root / "learn";
    o.skills_root = root / "skills";
    o.home_dir = "/home/nobody";
    o.project_dir = root.string();
    o.curate_every = std::chrono::seconds(3600);
    o.max_drafts_per_day = 3;
    const int64_t day0 = static_cast<int64_t>(std::time(nullptr)) / 86400;
    auto svc = LearnService::start(o);
    chk(svc != nullptr, "burst: the service starts");
    if (!svc) return;
    for (int p = 0; p < 12; ++p) {
        const Problem pr = problem(100 + p * 3 + 1);   // none of the planted-secret ones
        const uint32_t turn = static_cast<uint32_t>(p + 1);
        svc->sink()->emit(mk(T::TurnStart, turn, 0, kT0));
        for (uint32_t r = 1; r <= 6; ++r) {
            know::Event c = mk(T::Calls, turn, r, kT0);
            c.call.tool = "run_terminal";
            c.call.args = pr.cmd;
            c.call.binaries = {pr.binary};
            c.call.failed = r <= 2;
            c.call.error = r <= 2 ? pr.error : "";
            c.key = c.call.error;
            c.call.digest = pr.cmd + std::to_string(r);
            svc->sink()->emit(std::move(c));
        }
        svc->sink()->emit(mk(T::TurnEnd, turn, 6, kT0));
    }
    chk(svc->wait_idle(std::chrono::seconds(10)), "burst: every event is processed");
    const auto st = svc->status();
    svc->stop();
    const int64_t days = static_cast<int64_t>(std::time(nullptr)) / 86400 - day0 + 1;   // a midnight in the middle allows a second day's worth
    chk(st.drafts >= 1 && st.drafts <= static_cast<uint64_t>(3 * days), "burst: a flood of solved problems drafts within the daily cap (drafted " + std::to_string(st.drafts) + ", refused " + std::to_string(st.rejected) + ")");
    std::printf("burst: 12 solved problems in one day -> %llu skills drafted (cap 3), %llu refused, dropped %llu\n", (unsigned long long)st.drafts, (unsigned long long)st.rejected, (unsigned long long)st.dropped);
}

int main() {
    g_rng ^= static_cast<uint64_t>(env_long("DD_SOAK_SEED", 0)) * 0x2545F4914F6CDD1Dull;
    const int days = static_cast<int>(env_long("DD_SOAK_DAYS", 180));
    const int turns = static_cast<int>(env_long("DD_SOAK_TURNS", 300));
    const int crashes = static_cast<int>(env_long("DD_SOAK_CRASHES", 40));
    std::error_code ec;
    fs::remove_all(fs::path("build") / "learn_soak_tmp", ec);

    test_crashes(crashes);
    test_sim(days, turns);
    test_service_burst();

    fs::remove_all(fs::path("build") / "learn_soak_tmp", ec);
    if (g_fail) std::printf("%d FAILED\n", g_fail);
    else std::printf("learn soak: all passed\n");
    return g_fail ? 1 : 0;
}
