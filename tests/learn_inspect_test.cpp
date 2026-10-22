// dd learn: status, why, export and forget over a ledger with known records.
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "dd/core/path.hpp"
#include "dd/learn/inspect.hpp"
#include "dd/learn/ledger.hpp"

namespace fs = std::filesystem;
using namespace dd;
using namespace dd::learn;

static int g_fail = 0;
static void chk(bool ok, const std::string& label) {
    if (!ok) { ++g_fail; std::printf("FAIL %s\n", label.c_str()); }
}
static bool has(const std::string& h, const std::string& n) { return h.find(n) != std::string::npos; }

static know::Uri skill(const std::string& id) {
    know::Uri u;
    u.kind = know::Kind::Skill;
    u.id = id;
    return u;
}

static void write_skill(const fs::path& root, const std::string& cat, const std::string& name, bool learned) {
    fs::create_directories(root / cat / name);
    std::ofstream(root / cat / name / "SKILL.md") << "---\nname: " << name << "\ndescription: x\n" << (learned ? "origin: learned\nstate: probation\n" : "")
                                                  << "---\nbody of " << name << "\n";
}

static size_t visible_skills(const fs::path& root) {
    size_t n = 0;
    core::walk(root, "SKILL.md", core::WalkOptions{}, [&](const fs::path&) { ++n; });
    return n;
}

int main() {
    const fs::path base = fs::path("build") / ("learn_inspect_tmp_" + std::to_string(::getpid()));
    fs::remove_all(base);
    InspectPaths p{base / "learn", base / "skills"};
    const int64_t t0 = 1700000000, day = 86400;

    write_skill(p.skills_root, "ops", "fix-quadlet", true);
    write_skill(p.skills_root, "ops", "fix-other", true);
    write_skill(p.skills_root, "devops", "human-made", false);

    {
        std::string err;
        auto l = Ledger::open(p.learn_dir, {}, &err);
        chk(l != nullptr, "ledger opens: " + err);
        l->record(Record{StateRec{skill("fix-quadlet"), State::Draft, State::Probation, "template", t0}});
        for (uint32_t ep = 1; ep <= 3; ++ep) {
            OutcomeRec o;
            o.uri = skill("fix-quadlet");
            o.outcome = Outcome::Helped;
            o.terms = {"podman", "unit"};
            o.episode = ep;
            o.ts = t0 + day * ep;
            l->record(Record{o});
        }
        OutcomeRec f;
        f.uri = skill("fix-other");
        f.outcome = Outcome::Failed;
        f.ts = t0 + day;
        l->record(Record{f});
        l->sync();
    }
    const int64_t now = t0 + 4 * day;

    // ---- status ----
    const Result st = status(p, now);
    chk(st.ok && has(st.text, "learned skills: 2") && has(st.text, "probation 1"), "status counts learned skills by state: " + st.text);
    chk(has(st.text, "ops/fix-quadlet") && has(st.text, "efficacy"), "status lists a learned skill with its efficacy");
    chk(!has(st.text, "human-made"), "status never lists a skill that was not learned");

    // ---- why ----
    const Result w = why(p, "fix-quadlet", now);
    chk(w.ok && has(w.text, "state: probation") && has(w.text, "efficacy lower bound") && has(w.text, "helped 2.9"), "why shows state and efficacy: " + w.text);
    chk(has(w.text, "podman") && has(w.text, "episodes 3") && has(w.text, "* podman"), "why shows the confirmed term as promoted: " + w.text);
    chk(!why(p, "skill://nothing", now).ok, "why: an unknown skill says so");

    // ---- export is stable ----
    const Result e1 = export_jsonl(p, now), e2 = export_jsonl(p, now);
    chk(e1.ok && e1.text == e2.text && has(e1.text, "\"type\":\"card\"") && has(e1.text, "\"type\":\"assoc\"") && has(e1.text, "\"term\":\"podman\""), "export is stable JSON lines");

    // ---- read-only: a view changes nothing on disk ----
    const auto size_before = [&] {
        size_t n = 0;
        for (const auto& f : fs::directory_iterator(p.learn_dir)) n += fs::file_size(f.path());
        return n;
    };
    const size_t before = size_before();
    status(p, now);
    why(p, "fix-quadlet", now);
    chk(size_before() == before, "the views write nothing");
    chk(!status(InspectPaths{base / "nowhere", p.skills_root}, now).ok, "no ledger: an error, and no directory created");
    chk(!fs::exists(base / "nowhere"), "... and nothing was created");

    // ---- forget ----
    const Result refuse = forget(p, "human-made", now);
    chk(!refuse.ok && has(refuse.text, "not a learned skill") && fs::exists(p.skills_root / "devops" / "human-made" / "SKILL.md"), "a skill that was not learned is refused and untouched");
    chk(!forget(p, "skill://absent", now).ok, "forget: an unknown skill is refused");
    chk(visible_skills(p.skills_root) == 3, "three skills visible before");
    const Result fg = forget(p, "fix-quadlet", now);
    chk(fg.ok, "forget works: " + fg.text);
    chk(!fs::exists(p.skills_root / "ops" / "fix-quadlet") && fs::exists(p.skills_root / ".retired" / "ops" / "fix-quadlet" / "SKILL.md"), "the folder was moved, not deleted");
    chk(visible_skills(p.skills_root) == 2, "the retired skill is no longer found by the scanner");
    Stats s;
    chk(Ledger::load_readonly(p.learn_dir, s) && s.cards.count("skill://fix-quadlet") == 0, "the tombstone is honored after a replay");
    for (const auto& [k, a] : s.assoc) chk(!has(k, "fix-quadlet"), "... and its associations are gone");
    chk(s.cards.count("skill://fix-other") == 1, "... other cards stay");
    const Result again = forget(p, "fix-quadlet", now);
    chk(!again.ok, "forgetting twice is refused");

    fs::remove_all(base);
    if (g_fail) std::printf("%d FAILED\n", g_fail);
    else std::printf("learn inspect: all passed\n");
    return g_fail ? 1 : 0;
}
