// dd::know — when to look something up: signals, triggers, the working set, the router, the notes.
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>

#include "dd/context/memory.hpp"
#include "dd/know/deliver.hpp"
#include "dd/know/memory_catalogue.hpp"
#include "dd/know/policy.hpp"
#include "dd/know/prompt.hpp"
#include "dd/know/resolver.hpp"
#include "dd/know/router.hpp"
#include "dd/know/signal.hpp"
#include "dd/know/skills.hpp"
#include "dd/know/trigger.hpp"
#include "dd/know/working_set.hpp"

namespace fs = std::filesystem;
using namespace dd;
using namespace dd::know;

static int g_fail = 0;
static void chk(bool ok, const std::string& label) {
    if (!ok) { ++g_fail; std::printf("FAIL %s\n", label.c_str()); }
}
static bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

// ---- a pod stand-in: curated rows, a slow recall, labels and gets ----
struct FakePod : context::Memory {
    std::vector<context::Hit> rows;
    std::atomic<bool> is_up{true};
    std::atomic<int> recalls{0}, gets{0};
    std::atomic<int> recall_ms{0};
    std::vector<context::Hit> recall_hits;
    std::set<long> hidden;   // ids a flaky listing leaves out
    void add(const std::string& label, const std::string& content) {
        context::Hit h;
        h.id = static_cast<long>(rows.size()) + 500;
        h.label = label;
        h.content = content;
        rows.push_back(h);
    }
    bool configured() const override { return true; }
    bool up() const override { return is_up.load(); }
    bool probe() override { return is_up.load(); }
    std::vector<context::Hit> recall(const std::string&, int, const std::string&) override {
        ++recalls;
        std::this_thread::sleep_for(std::chrono::milliseconds(recall_ms.load()));
        return recall_hits;
    }
    long remember(const std::string&, const std::string&) override { return -1; }
    std::vector<context::Hit> think(long, int) override { return {}; }
    std::vector<context::Hit> facts(long, int) override { return {}; }
    std::string intent(const std::string&) override { return "general"; }
    int connect(const std::vector<context::Edge>&) override { return 0; }
    std::vector<context::Hit> labels(const std::string& prefix, int offset, int limit) override {
        std::vector<context::Hit> out;
        if (!is_up) return out;
        int skip = offset;
        for (const auto& r : rows) {
            if (r.label.rfind(prefix, 0) != 0 || hidden.count(r.id)) continue;
            if (skip-- > 0) continue;
            if (static_cast<int>(out.size()) >= limit) break;
            context::Hit h;
            h.id = r.id;
            h.label = r.label;
            out.push_back(h);
        }
        return out;
    }
    std::vector<context::Hit> get(const std::vector<long>& ids) override {
        ++gets;
        std::vector<context::Hit> out;
        if (!is_up) return out;
        for (long id : ids)
            for (const auto& r : rows)
                if (r.id == id) out.push_back(r);
        return out;
    }
};

static void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

static std::string skill_md(const std::string& name, const std::string& desc, const std::string& tags, const std::string& body) {
    return "---\nname: " + name + "\ndescription: " + desc + "\ntags: [" + tags + "]\n---\n\n# " + name + "\n\n" + body;
}

static core::ToolCall call(const std::string& tool, const std::string& args, const std::string& id = "c1") {
    core::ToolCall c;
    c.id = id;
    c.name = tool;
    c.arguments = args;
    return c;
}

// ================= call records =================
static void test_records() {
    auto r = make_record(call("run_terminal", "{\"cmd\":\"sudo podman ps -a | grep quadlet\",\"intent\":\"Checking the quadlet units\"}"),
                         "{\"cmd\":\"podman ps\",\"exit\":0,\"stdout\":\"CONTAINER ID  IMAGE\\n\"}");
    chk(r.tool == "run_terminal" && r.intent == "Checking the quadlet units", "record: tool and intent");
    chk(r.binaries.size() == 2 && r.binaries[0] == "podman" && r.binaries[1] == "grep", "record: the programs of a pipeline, sudo skipped");
    chk(!r.failed && !r.empty, "record: a call that printed something is neither failed nor empty");
    chk(r.digest.find("intent") == std::string::npos, "record: the digest leaves the intent out");

    auto a = make_record(call("run_terminal", "{\"cmd\":\"ls /srv/x\",\"intent\":\"one\"}"), "{\"exit\":0,\"stdout\":\"\"}");
    auto b = make_record(call("run_terminal", "{\"cmd\":\"ls /srv/x\",\"intent\":\"two\"}"), "{\"exit\":0,\"stdout\":\"\"}");
    chk(a.digest == b.digest && a.exploring && a.empty, "record: same call, other intent: same digest; ls with no output is a blind look");

    auto held = make_record(call("run_terminal", "{\"cmd\":\"docker ps\"}"),
                            "{\"error\":\"[rule podman-not-docker · told once] This box runs podman.\\nThis call was not run. Call again once you have taken the rule into account.\"}");
    chk(held.refused && !held.failed && held.error.empty() && !held.exploring, "record: a call a rule held is refused: not failed, not blind, nothing to look up");
    auto f = make_record(call("run_terminal", "{\"cmd\":\"make build/dd_tui\"}"),
                         "{\"cmd\":\"make\",\"exit\":2,\"stdout\":\"src/a.cpp:312: error: 'foo' was not declared\\nmake: *** [Makefile:67: x] Error 1\"}");
    chk(f.failed && f.exit == 2 && !f.error.empty(), "record: a nonzero exit fails, with an error");
    chk(f.error.find("312") == std::string::npos && f.error.find("#") != std::string::npos, "record: numbers are folded out of the signature: " + f.error);
    auto g = make_record(call("run_terminal", "{\"cmd\":\"make build/dd_tui\"}"),
                         "{\"cmd\":\"make\",\"exit\":2,\"stdout\":\"src/b.cpp:77: error: 'foo' was not declared\\nmake: *** [Makefile:67: x] Error 1\"}");
    chk(f.error == g.error, "record: the same error on another line is the same signature");

    auto m = make_record(call("read_file", "{\"path\":\"/home/x/missing.cpp\"}"), "{\"error\":\"cannot open /home/x/missing.cpp\"}");
    chk(m.failed && m.exploring && m.empty && m.paths.size() == 1, "record: a read that cannot open is a blind look");
    auto d = make_record(call("list_dir", "{\"path\":\"/srv\"}"), "{\"path\":\"/srv\",\"entries\":[]}");
    chk(d.exploring && d.empty && !d.failed, "record: an empty listing found nothing");
    auto ok = make_record(call("read_file", "{\"path\":\"/srv/a.cpp\"}"), "{\"path\":\"/srv/a.cpp\",\"content\":\"int x;\"}");
    chk(!ok.exploring && !ok.empty, "record: a read that worked is progress, not exploring");
    auto mcp = make_record(call("mazemaker_recall", "{\"query\":\"x\"}"), "[{\"id\":1}]");
    chk(!mcp.failed && !mcp.empty && !mcp.exploring, "record: another tool's result is taken as it is");

    chk(error_signature("").empty(), "signature: nothing");
    chk(error_signature("all fine\nreally fine\nstill fine") == "still fine", "signature: no cue: the last line");
    chk(error_signature("ok\nerror: cannot open /a/b/c.txt:12\nok") == "error: cannot open <path>:#", "signature: " + error_signature("ok\nerror: cannot open /a/b/c.txt:12\nok"));
    chk(error_signature("make: *** [Makefile:67: build/dd_tui] Error 1") == error_signature("make: *** [Makefile:12: build/dd_maze_tests] Error 2"), "signature: line numbers and build targets fold");
}

// ================= the working set =================
static void test_working_set() {
    WorkingSet ws;
    Uri skill, other;
    skill.id = "podman-deep-dive";
    other.id = "git-branch-workflow";
    ws.new_turn("q1");
    ws.offered(skill, "podman-deep-dive", 40);
    chk(ws.offered_this_turn(skill) && ws.offered_this_epoch(skill) && !ws.offered_this_turn(other), "ws: offered this turn and epoch");
    ws.new_turn("q2");
    chk(!ws.offered_this_turn(skill) && ws.offered_this_epoch(skill), "ws: a new turn may offer it again, the epoch remembers it");
    ws.new_epoch(1);
    chk(!ws.offered_this_epoch(skill), "ws: a new epoch forgets");

    ws.opened(skill, "podman-deep-dive", 900);
    chk(ws.open_in_window(skill), "ws: opened in this epoch: its text is in the window");
    ws.new_epoch(2);
    chk(!ws.open_in_window(skill), "ws: after a rebase its text is gone");
    chk(ws.in_use(Kind::Skill, 2).size() == 1 && ws.in_use(Kind::Skill, 2)[0].title == "podman-deep-dive", "ws: ... but the skill is still the one in use");
    ws.new_turn("q3");
    ws.new_turn("q4");
    chk(ws.in_use(Kind::Skill, 2).empty(), "ws: not in use any more after two turns");

    // topics
    ws.new_turn("q5");
    std::vector<Term> t = {{"podman", 1}, {"quadlet", 1}, {"systemctl", 1}};
    chk(ws.uncovered(t) == 3, "ws: nothing covered at first");
    ws.cover({{"podman", 1}});
    chk(ws.uncovered(t) == 2 && ws.covered("podman"), "ws: covered terms are not new");

    // notes
    chk(ws.rounds_since_note() > 100, "ws: no note yet this turn");
    ws.new_round();
    ws.noted();
    ws.new_round();
    chk(ws.rounds_since_note() == 1, "ws: rounds since the last note");

    // what the model has been doing
    auto rec = [](const char* tool, const char* args, bool exploring, bool empty, bool failed = false, const char* err = "") {
        CallRecord c;
        c.tool = tool;
        c.digest = std::string(tool) + "|" + args;
        c.exploring = exploring;
        c.empty = empty;
        c.failed = failed;
        c.error = err;
        return c;
    };
    ws.new_turn("q6");
    ws.record({rec("run_terminal", "a", false, false), rec("read_file", "x", false, false)});
    chk(ws.identical_streak() == 1 && ws.blind_streak() == 0, "ws: two different calls");
    ws.record({rec("read_file", "x", false, false), rec("read_file", "x", false, false)});
    chk(ws.identical_streak() == 3, "ws: the same call three times in a row");
    ws.new_turn("q7");
    ws.record({rec("list_dir", "/a", true, true), rec("run_terminal", "ls /b", true, true, true, "no such file"), rec("list_dir", "/c", true, true)});
    chk(ws.blind_streak() == 3 && ws.last_blind_summary().find("list_dir x2") != std::string::npos, "ws: three looks around that found nothing: " + ws.last_blind_summary());
    ws.record({rec("write_file", "f", false, false)});
    chk(ws.blind_streak() == 0, "ws: progress ends the streak");
    ws.record({rec("run_terminal", "b", false, true, true, "error: x #"), rec("run_terminal", "c", false, true, true, "error: x #")});
    chk(ws.failures("error: x #") == 2 && ws.family_failures("run_terminal") == 3, "ws: failures counted by signature and by tool");
    chk(!ws.was_warned("error: x #"), "ws: not spoken to yet");
    ws.warn("error: x #");
    chk(ws.was_warned("error: x #"), "ws: spoken to");
    ws.new_turn("q8");
    chk(ws.failures("error: x #") == 0 && !ws.stalled(), "ws: a new turn starts clean");
}

// ================= triggers (pure) =================
struct Fixture {
    Policy policy;
    Distiller distill;
    WorkingSet ws;
    IdfFn idf = [](const std::string&) { return 2.0f; };   // every term is known to the index
    Plan plan(const Signal& s, std::vector<std::unique_ptr<Trigger>> triggers = default_triggers()) {
        Plan out;
        const PlanContext c{ws, policy, distill, idf};
        for (const auto& t : triggers) t->plan(s, c, out);
        return out;
    }
};

static void test_triggers() {
    Fixture f;
    TurnStarted t;
    t.text = "restart the quadlet unit for the pod";
    t.anchored = t.text;
    auto p = f.plan(t);
    chk(p.size() == 1 && p[0].reason == "turn" && p[0].deep && p[0].where == Placement::QuestionNote && p[0].kinds.size() == 3, "turn: one intent, deep, on the question");
    chk(p[0].wait_ms == f.policy.deep_ms, "turn: waits the short deadline for an action");
    chk(!p[0].query.terms.empty() && p[0].query.text == t.text, "turn: a distilled query, the words as written for the deep leg");

    t.text = "what did we decide about the dream engine last time?";
    t.anchored = t.text;
    p = f.plan(t);
    chk(p.size() == 1 && p[0].wait_ms == f.policy.deep_recall_ms, "turn: a question about the past waits for the pod");
    chk(asks_about_the_past("wie war das nochmal, haben wir das beschlossen?") && !asks_about_the_past("fix the build"), "turn: past-tense cues, English and German");

    t.trivial = true;
    chk(f.plan(t).empty(), "turn: ok, thanks: nothing to look up");
    t.trivial = false;
    t.skip_deep = true;
    p = f.plan(t);
    chk(p.size() == 1 && !p[0].deep, "turn: right after a rebase the pod was just asked: pointers only");

    // shift: a topic the turn has not covered
    Fixture s;
    s.ws.new_turn("fix the tui");
    s.ws.new_round();
    CallsFinished cf;
    CallRecord c1;
    c1.tool = "run_terminal";
    c1.intent = "Checking the quadlet units";
    c1.binaries = {"systemctl", "podman"};
    c1.paths = {"/home/dev/projects/mazemaker/quadlet/db.container"};
    cf.calls = {c1};
    p = s.plan(cf);
    chk(!p.empty() && p.back().reason.rfind("shift: ", 0) == 0 && p.back().where == Placement::AfterTools && !p.back().deep, "shift: new terms after a batch: a local lookup after the tools");
    s.ws.cover(p.back().query.terms);
    s.ws.new_round();
    p = s.plan(cf);
    chk(p.empty(), "shift: the same topic again says nothing");

    Fixture gap;
    gap.ws.new_turn("q");
    gap.ws.new_round();
    gap.ws.noted();
    gap.ws.new_round();
    chk(gap.plan(cf).empty(), "shift: not on the heels of the last note");
    Fixture off;
    off.policy.shift = false;
    off.ws.new_turn("q");
    off.ws.new_round();
    chk(off.plan(cf).empty(), "shift: mid_turn.shift: false switches it off");
    Fixture noroute;
    noroute.policy.auto_route = false;
    noroute.ws.new_turn("q");
    noroute.ws.new_round();
    chk(noroute.plan(cf).empty(), "shift: skills.auto_route: false switches it off");

    // failure
    Fixture fl;
    fl.ws.new_turn("build it");
    fl.ws.new_round();
    CallRecord bad;
    bad.tool = "run_terminal";
    bad.failed = true;
    bad.error = "error: undefined reference to #";
    bad.binaries = {"make"};
    CallsFinished fc;
    fc.calls = {bad};
    fl.ws.record(fc.calls);
    p = fl.plan(fc);
    bool failure = false, deep = true;
    for (const auto& in : p)
        if (in.reason.rfind("failure", 0) == 0) {
            failure = true;
            deep = in.deep;
            chk(in.key == bad.error && in.where == Placement::AfterTools, "failure: keyed by the signature, after the tools");
        }
    chk(failure && !deep, "failure: a first failure: a local lookup, not the pod");
    fl.ws.record(fc.calls);
    p = fl.plan(fc);
    deep = false;
    for (const auto& in : p)
        if (in.reason.rfind("failure", 0) == 0) deep = in.deep;
    chk(deep, "failure: a repeat is worth the pod's time");
    fl.ws.warn(bad.error);
    bool again = false;
    for (const auto& in : fl.plan(fc))
        if (in.reason.rfind("failure", 0) == 0) again = true;
    chk(!again, "failure: once spoken to, not again this turn");
    CallRecord rule;
    rule.tool = "run_terminal";
    rule.failed = true;
    rule.error = "[rule podman-not-docker] use podman";
    CallsFinished rc;
    rc.calls = {rule};
    Fixture rf;
    rf.ws.new_turn("q");
    rf.ws.new_round();
    rf.ws.record(rc.calls);
    bool spoke = false;
    for (const auto& in : rf.plan(rc))
        if (in.reason.rfind("failure", 0) == 0) spoke = true;
    chk(!spoke, "failure: a rule's own refusal is not an error to look up");

    // stall
    Fixture st;
    st.ws.new_turn("find the loader");
    st.ws.new_round();
    CallRecord same;
    same.tool = "read_file";
    same.digest = "read_file|{\"path\":\"/a\"}";
    st.ws.record({same});
    CallsFinished sc;
    sc.calls = {same};
    chk(st.plan(sc).empty() || st.plan(sc).front().reason.rfind("stall", 0) != 0, "stall: one call is not a loop");
    st.ws.record({same});
    p = st.plan(sc);
    bool stalled = false;
    for (const auto& in : p)
        if (in.reason == "stall: same call") {
            stalled = true;
            chk(in.notice && in.deep && in.query.text == "find the loader", "stall: a notice, the question as the query, the pod too");
        }
    chk(stalled, "stall: the same call twice");
    st.ws.set_stalled();
    bool twice = false;
    for (const auto& in : st.plan(sc))
        if (in.reason.rfind("stall", 0) == 0) twice = true;
    chk(!twice, "stall: once per turn");
    chk(StallTrigger::notice_text(st.ws, st.policy).find("same call") != std::string::npos, "stall: the notice names what happened");

    Fixture bl;
    bl.ws.new_turn("where is the config loader");
    bl.ws.new_round();
    for (const char* path : {"/a", "/b", "/c"}) {
        CallRecord b;
        b.tool = "list_dir";
        b.digest = std::string("list_dir|") + path;
        b.exploring = true;
        b.empty = true;
        bl.ws.record({b});
    }
    CallsFinished bc;
    bc.calls = {CallRecord{}};
    bool blind = false;
    for (const auto& in : bl.plan(bc))
        if (in.reason == "stall: blind") blind = true;
    chk(blind, "stall: three looks around that found nothing");
    chk(StallTrigger::notice_text(bl.ws, bl.policy).find("STOP guessing") != std::string::npos, "stall: the notice says to stop guessing");
}

// ================= notes =================
static Hit hit(const std::string& name, Kind kind, double score, const std::string& gist, bool body = true) {
    Hit h;
    h.card.uri.kind = kind;
    h.card.uri.id = name;
    h.card.title = name;
    h.card.gist = gist;
    h.card.has_body = body;
    h.score = score;
    return h;
}

static void test_deliverer() {
    Deliverer d;
    Shown s;
    s.skills = {hit("podman-deep-dive", Kind::Skill, 0.71, "Debug podman networking and symlink mess"), hit("podman-quadlet-ops", Kind::Skill, 0.64, "Operate quadlet units")};
    s.memory = {hit("1290233", Kind::Memory, 0.5, "never touch live pods")};
    s.memory[0].card.title = "invariant:podman";
    std::vector<Uri> offered;
    bool head_in = true;
    std::string n = d.note(s, "turn", 600, 1500, &offered, &head_in);
    chk(has(n, "<retrieval why=\"turn\">") && has(n, "</retrieval>"), "note: wrapped");
    chk(has(n, "podman-deep-dive 0.71") && has(n, "read_file(\"skill://podman-deep-dive\")"), "note: a pointer names the card, the score and how to open it");
    chk(has(n, "memory #1290233 [invariant:podman]") && has(n, "read_file(\"mem://1290233\")"), "note: a memory pointer carries its id");
    chk(offered.size() == 3 && !head_in, "note: the cards that made it in");

    Hit bodyless = hit("lost", Kind::Skill, 0.5, "a card without a body", false);
    Shown b;
    b.skills = {bodyless};
    chk(has(d.note(b, "turn", 600, 600), "(no body to open)"), "note: a card with no body says so instead of pointing nowhere");
    Hit tool = hit("btquant_run", Kind::Tool, 0.5, "run a backtest");
    Shown tl;
    tl.tools = {tool};
    chk(has(d.note(tl, "turn", 600, 600), "tool_describe(\"btquant_run\")"), "note: a deferred tool is opened with tool_describe");

    // a head opened for the model
    Body body;
    body.uri.id = "podman-deep-dive";
    body.text = "# podman-deep-dive\nStep one.\n";
    body.page = 1;
    body.pages = 3;
    body.more = "skill://podman-deep-dive#2";
    s.head = body;
    s.head_title = "podman-deep-dive";
    n = d.note(s, "turn", 600, 1500, &offered, &head_in);
    chk(head_in && has(n, "<skill name=\"podman-deep-dive\" page=\"1/3\">") && has(n, "Step one.") && has(n, "more: read_file(\"skill://podman-deep-dive#2\")"),
        "note: the head is opened for the model, with where it goes on");
    n = d.note(s, "turn", 600, 20, &offered, &head_in);
    chk(!head_in && !has(n, "<skill name"), "note: a head that does not fit the budget is not opened");

    // the pointer budget cuts at a line, never in the middle of one
    Shown many;
    for (int i = 0; i < 20; ++i) many.skills.push_back(hit("skill-" + std::to_string(i), Kind::Skill, 0.5, std::string(100, 'g')));
    offered.clear();
    n = d.note(many, "turn", 120, 1500, &offered, nullptr);
    chk(offered.size() > 0 && offered.size() < 10 && n.back() == '>', "note: the pointer budget keeps the note short");
    chk(d.note(Shown{}, "turn", 600, 600).empty(), "note: nothing to say is no note");

    // the ratio: tokens as the server counts them
    Deliverer heavy([] { return 2.0; });
    chk(heavy.tokens(std::string(400, 'x')) == 200 && d.tokens(std::string(400, 'x')) == 100, "tokens: chars/4 at the measured ratio, never under");
    Deliverer light([] { return 0.5; });
    chk(light.tokens(std::string(400, 'x')) == 100, "tokens: a ratio under 1 is not believed");

    chk(has(Deliverer::notice("blind_exploration", "stop"), "<system-notice reason=\"blind_exploration\">"), "notice: a system notice");
    WorkingSet ws;
    Uri u;
    u.id = "podman-deep-dive";
    ws.new_turn("q");
    ws.opened(u, "podman-deep-dive", 900);
    const auto r = Deliverer::reattach(ws.in_use(Kind::Skill, 2));
    chk(has(r, "<retrieval why=\"rebase\">") && has(r, "podman-deep-dive") && has(r, "left the window") && has(r, "read_file(\"skill://podman-deep-dive\")"),
        "reattach: the skill in use, as a pointer");
    chk(Deliverer::reattach({}).empty(), "reattach: nothing in use, nothing said");
}

// ================= the router =================
struct Stack {
    fs::path root;
    std::shared_ptr<FakePod> pod = std::make_shared<FakePod>();
    std::shared_ptr<SkillCatalogue> skills;
    std::shared_ptr<MemoryCatalogue> memory;
    std::unique_ptr<Router> router;
    std::vector<std::string> logs;
};

static std::unique_ptr<Stack> make_stack(const fs::path& tmp, const std::string& name, Policy p = {}) {
    auto s = std::make_unique<Stack>();
    s->root = tmp / name;
    const fs::path sk = s->root / "skills";
    write(sk / "devops" / "podman-deep-dive" / "SKILL.md",
          skill_md("podman-deep-dive", "Debug podman networking, storage and symlink mess in containers", "podman, containers, symlink",
                   "Check the container storage first.\n\n" + std::string(6000, 'p') + "\n"));
    write(sk / "devops" / "podman-quadlet-ops" / "SKILL.md",
          skill_md("podman-quadlet-ops", "Operate rootless podman containers and quadlet systemd units: restart, logs, drop-ins", "podman, quadlet, systemd", "Quadlet units live in the config dir.\n"));
    write(sk / "software-development" / "git-branch-workflow" / "SKILL.md",
          skill_md("git-branch-workflow", "Branch, rebase and merge safely; short commits", "git, branch, rebase", "Rebase before you merge.\n"));
    write(sk / "creative" / "songwriting" / "SKILL.md", skill_md("songwriting", "Write song lyrics and structure verses", "music, lyrics", "Verses and a chorus.\n"));
    s->pod->add("invariant:podman-symlinks", "Never replace a podman quadlet symlink with a copy: the units are linked from the repo.\n\nThe quadlet directory holds symlinks on purpose.");
    s->pod->add("bug:quadlet-undefined-reference", "error: undefined reference to quadlet helper after the update; fixed by relinking the quadlet generator");
    s->pod->add("decision:song-format", "Songs are written as plain lyrics files");
    s->skills = std::make_shared<SkillCatalogue>(std::vector<std::shared_ptr<SkillSource>>{std::make_shared<DiskSkills>(std::vector<fs::path>{sk})});
    s->memory = std::make_shared<MemoryCatalogue>(s->pod, s->root / "cache" / "memory-cards.jsonl");
    s->skills->refresh();
    s->memory->refresh();
    // small test catalogues: the calibrated thresholds are for 568 skills; the rule's shape is what is under test
    p.skills = Confidence{2, 2.0, 0.15, 0.30, 0};
    p.memory = Confidence{2, 2.0, 0.15, 2.0, 0};
    p.deep_ms = 60;
    p.deep_recall_ms = 400;
    s->router = std::make_unique<Router>(Router::Catalogues{s->skills, s->memory, nullptr}, p, s->pod);
    Stack* raw = s.get();
    s->router->on_log = [raw](const std::string& l) { raw->logs.push_back(l); };
    return s;
}

static TurnStarted turn(const std::string& text, int epoch = 0) {
    TurnStarted t;
    t.text = text;
    t.anchored = text;
    t.epoch = epoch;
    return t;
}

static void test_memory_catalogue(const fs::path& tmp) {
    auto s = make_stack(tmp, "memcat");
    chk(s->memory->ready() && s->memory->size() == 3, "memory: three curated rows are cards");
    Distiller d;
    const auto hits = s->memory->search(d.text("the podman quadlet symlink", [&](const std::string& t) { return s->memory->idf(t); }), 3);
    chk(!hits.empty() && hits[0].card.title == "invariant:podman-symlinks", "memory: found by what the row says, locally");
    Uri u;
    u.kind = Kind::Memory;
    u.id = hits.empty() ? "0" : hits[0].card.uri.id;
    const auto body = s->memory->open(u);
    chk(body && has(body->text, "[invariant:podman-symlinks]") && has(body->text, "symlinks on purpose"), "memory: opening is a primary-key read, label first");
    chk(has(s->memory->map(), "memory 3 curated"), "memory: the map");

    // the cache beside the binary: the next run knows the rows before the pod answers
    chk(fs::exists(s->root / "cache" / "memory-cards.jsonl"), "memory: cards are cached on disk");
    auto pod2 = std::make_shared<FakePod>();
    pod2->is_up = false;
    MemoryCatalogue cold(pod2, s->root / "cache" / "memory-cards.jsonl");
    cold.refresh();
    chk(cold.ready() && cold.size() == 3 && !cold.healthy(), "memory: pod down at launch: searchable from the cache, not healthy");
    const int gets_before = s->pod->gets.load();
    s->memory->refresh();
    chk(s->pod->gets.load() == gets_before, "memory: an unchanged maze costs no row reads, only labels");
    s->pod->add("fact:new-row", "a row written during the session about quadlet generators");
    s->memory->refresh();
    chk(s->memory->size() == 4 && s->pod->gets.load() == gets_before + 1, "memory: a new row is fetched alone, by id");
    chk(MemoryCatalogue::gist_of_row("first paragraph\n\nsecond") == "first paragraph", "memory: the gist is the first paragraph");

    // a pod that flakes mid-listing is not a maze that lost its rows
    const size_t known = s->memory->size();
    for (const auto& r : s->pod->rows) s->pod->hidden.insert(r.id);
    s->pod->hidden.erase(s->pod->rows.front().id);   // one row of many came back
    s->memory->refresh();
    chk(s->memory->size() == known && !s->memory->healthy(), "memory: a listing far shorter than what is known changes nothing and says unhealthy");
    s->pod->hidden.clear();
    s->memory->refresh();
    chk(s->memory->healthy() && s->memory->size() == known, "memory: the next whole listing is healthy again");
}

static void test_router_turn(const fs::path& tmp) {
    auto s = make_stack(tmp, "turn");
    s->pod->recall_hits = {};
    auto d = s->router->on(turn("restart the podman quadlet units, the symlink mess is back"));
    chk(has(d.note, "<retrieval why=\"turn\">") && has(d.note, "podman-quadlet-ops"), "router: a question gets pointers from the catalogue at once: " + d.note.substr(0, 200));
    chk(has(d.note, "invariant:podman-symlinks"), "router: ... and the curated row that is about it");
    chk(!d.offered.empty() && d.skill != "", "router: what was offered is reported");
    chk(s->pod->recalls.load() == 1, "router: the pod's recall ran once");

    // the same question again in the same turn's epoch: not offered twice within a turn; a new turn offers again
    auto d2 = s->router->on(turn("restart the podman quadlet units, the symlink mess is back"));
    chk(has(d2.note, "podman-quadlet-ops") || has(d2.note, "podman-deep-dive"), "router: a new turn may offer the same pointer again");

    auto none = s->router->on(turn("thanks, that is all for today, see you tomorrow"));
    chk(none.note.empty(), "router: nothing in the catalogue is about it: no note, no false offer");

    TurnStarted triv = turn("ok");
    triv.trivial = true;
    const int recalls_before = s->pod->recalls.load();
    chk(s->router->on(triv).note.empty() && s->pod->recalls.load() == recalls_before, "router: a trivial turn looks nothing up (no pod call)");
    chk(!s->logs.empty() && has(s->logs[0], "\"signal\":\"turn\"") && has(s->logs[0], "skill://"), "router: a log line says what was offered");
}

static void test_router_open_head(const fs::path& tmp) {
    Policy p;
    auto s = make_stack(tmp, "head", p);
    auto d = s->router->on(turn("the podman symlink container mess needs to stop"));
    chk(!d.opened.empty() && d.opened[0].id == "podman-deep-dive", "router: a confident skill's head is opened for the model");
    chk(has(d.note, "<skill name=\"podman-deep-dive\" page=\"1/") && has(d.note, "Check the container storage first."), "router: the head is in the note");
    chk(has(d.note, "more: read_file(\"skill://podman-deep-dive#2\")"), "router: ... and the way to the rest");
    chk(s->router->stats().opened_for.load() == 1, "router: counted as opened for the model");
    // the skill is in the window now: a second ask in the same epoch does not open it again
    auto d2 = s->router->on(turn("and the podman symlink container mess again"));
    chk(d2.opened.empty() && !has(d2.note, "<skill name=\"podman-deep-dive\""), "router: a skill already in the window is not opened twice");
    // a rebase: its text is gone; the reattach block says so
    s->router->on(Rebased{1, "q"});
    const std::string r = s->router->reattach();
    chk(has(r, "podman-deep-dive") && has(r, "left the window"), "router: after a rebase the skill in use is pointed to again: " + r);
    auto d3 = s->router->on(turn("the podman symlink container mess, once more", 1));
    chk(!d3.opened.empty(), "router: in the new epoch the head may open again");
    // auto_open off: pointers only
    Policy q;
    q.auto_open = false;
    auto s2 = make_stack(tmp, "head2", q);
    auto e = s2->router->on(turn("the podman symlink container mess needs to stop"));
    chk(e.opened.empty() && has(e.note, "podman-deep-dive"), "router: knowledge.auto_open: false offers the pointer only");
    // a body too large to open for the model
    Policy big;
    big.open_max_bytes = 100;
    auto s3 = make_stack(tmp, "head3", big);
    chk(s3->router->on(turn("the podman symlink container mess needs to stop")).opened.empty(), "router: a skill past open_max_bytes is only pointed to");
}

static void test_router_budget(const fs::path& tmp) {
    Policy p;
    p.max_turn_tokens = 30;   // far too little
    auto s = make_stack(tmp, "budget", p);
    auto d = s->router->on(turn("the podman symlink container mess needs to stop"));
    chk(d.opened.empty(), "budget: a head that does not fit the turn's room is not opened");
    chk(Deliverer().tokens(d.note) <= 60, "budget: the note stays inside the room it was given (" + std::to_string(Deliverer().tokens(d.note)) + " tokens)");
    Policy none;
    none.auto_route = false;
    auto s2 = make_stack(tmp, "noroute", none);
    chk(s2->router->on(turn("the podman symlink container mess needs to stop")).note.empty(), "budget: skills.auto_route: false offers nothing on its own");
}

static void test_router_deep(const fs::path& tmp) {
    // the pod answers slowly: the turn does not wait past the deadline; the answer rides a later round
    auto s = make_stack(tmp, "deep");
    context::Hit h;
    h.id = 900;
    h.label = "decision:something";
    h.content = "an earlier decision about quadlet generators that matters here a lot";
    h.score = 0.7;
    s->pod->recall_hits = {h};
    s->pod->recall_ms = 400;
    const auto t0 = std::chrono::steady_clock::now();
    auto d = s->router->on(turn("restart the quadlet generator please"));
    const double waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    chk(waited < 300, "deep: the turn waited its deadline, not the pod (" + std::to_string(waited) + " ms)");
    chk(d.deep_late && !has(d.note, "an earlier decision"), "deep: a slow answer is marked late and not in the note");
    std::this_thread::sleep_for(std::chrono::milliseconds(450));
    const auto late = s->router->late();
    chk(late && has(late->note, "an earlier decision about quadlet generators"), "deep: the late answer is delivered with a later round");
    chk(!s->router->late(), "deep: once");
    chk(s->router->stats().deep_late.load() == 1, "deep: counted");

    // a question about the past waits for the pod
    auto s2 = make_stack(tmp, "deep2");
    s2->pod->recall_hits = {h};
    s2->pod->recall_ms = 150;
    auto past = s2->router->on(turn("what did we decide about the quadlet generator last time?"));
    chk(!past.deep_late && has(past.note, "an earlier decision about quadlet generators"), "deep: a question about the past waits for the pod's answer");

    // an interrupt stops the wait
    auto s3 = make_stack(tmp, "deep3");
    s3->pod->recall_hits = {h};
    s3->pod->recall_ms = 600;
    std::atomic<bool> stop{false};
    s3->router->cancelled = [&] { return stop.load(); };
    std::thread killer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        stop = true;
    });
    const auto t1 = std::chrono::steady_clock::now();
    s3->router->on(turn("what did we decide about the quadlet generator last time?"));
    const double w3 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    killer.join();
    chk(w3 < 300, "deep: an interrupted turn does not wait the pod out (" + std::to_string(w3) + " ms)");
    std::this_thread::sleep_for(std::chrono::milliseconds(650));   // let the detached job finish before its pod goes away

    // the pod down: pointers still come, from the catalogue
    auto s4 = make_stack(tmp, "deep4");
    s4->pod->is_up = false;
    auto down = s4->router->on(turn("restart the podman quadlet units"));
    chk(has(down.note, "podman-quadlet-ops") && s4->pod->recalls.load() == 0, "deep: pod down: the catalogue still answers, nothing waits");
}

static void test_router_midturn(const fs::path& tmp) {
    auto s = make_stack(tmp, "mid");
    s->router->on(turn("fix the songwriting skill"));
    CallsFinished cf;
    CallRecord c;
    c.tool = "run_terminal";
    c.intent = "Checking the quadlet units";
    c.binaries = {"podman"};
    c.paths = {"/srv/mazemaker/quadlet/db.container"};
    cf.calls = {c};
    auto d = s->router->on(cf);
    chk(has(d.note, "<retrieval why=\"shift:") && has(d.note, "podman-quadlet-ops"), "mid-turn: a shift to podman after a batch: a note after the tools: " + d.note.substr(0, 120));
    auto again = s->router->on(cf);
    chk(again.note.empty(), "mid-turn: the same topic next round: nothing");

    // a failure with a known error
    CallRecord bad;
    bad.tool = "run_terminal";
    bad.failed = true;
    bad.error = "error: undefined reference to quadlet helper";
    bad.binaries = {"make"};
    CallsFinished fc;
    fc.calls = {bad};
    s->router->on(CallsFinished{});   // advance a round
    s->router->on(CallsFinished{});
    auto f = s->router->on(fc);
    chk(has(f.note, "bug:quadlet-undefined-reference"), "mid-turn: a failure finds the maze's row about that error: " + f.note.substr(0, 160));

    // a stall: the same call twice
    CallRecord same;
    same.tool = "read_file";
    same.digest = "read_file|{\"path\":\"/a\"}";
    CallsFinished sc;
    sc.calls = {same};
    s->router->on(sc);
    auto st = s->router->on(sc);
    chk(has(st.note, "<system-notice reason=\"repeated_call\">") && has(st.note, "NEVER repeat it"), "mid-turn: the same call twice: a redirect notice");
    auto st2 = s->router->on(sc);
    chk(!has(st2.note, "system-notice"), "mid-turn: the notice is given once per turn");
}

static void test_router_tools(const fs::path& tmp) {
    auto s = make_stack(tmp, "tools");
    Resolver r(*s->router);

    auto j = core::json_parse(*r.read("skill://podman-quadlet-ops"));
    chk(core::json_get(j, "uri") && core::json_get(j, "text") && has(core::json_get(j, "text")->str, "Quadlet units live"), "read: skill:// opens a skill through the file verb");
    const auto big = core::json_parse(*r.read("skill://podman-deep-dive"));
    chk(core::json_get(big, "more") && has(core::json_get(big, "more")->str, "skill://podman-deep-dive#2") && core::json_get(big, "page")->str.rfind("1/", 0) == 0,
        "read: a long skill comes a page at a time and names the next");
    const auto p2 = core::json_parse(*r.read("skill://podman-deep-dive#2"));
    chk(core::json_get(p2, "page") && core::json_get(p2, "page")->str.rfind("2/", 0) == 0, "read: #2 is the second page");
    const auto mem = core::json_parse(*r.read("mem://" + std::to_string(500)));
    chk(core::json_get(mem, "text") && has(core::json_get(mem, "text")->str, "invariant:podman-symlinks"), "read: mem:// opens a curated row");
    chk(!r.read("/etc/passwd") && !r.read("src/main.cpp") && !r.read("https://x.org/a"), "read: a file path is not ours: nullopt, the file read goes on");
    const auto miss = core::json_parse(*r.read("skill://podman-quadlet"));
    chk(core::json_get(miss, "error") && has(core::json_get(miss, "error")->str, "close: podman-quadlet-ops"), "read: a near name gets suggestions");
    chk(!r.read("skill://x/../../etc/passwd"), "read: a path with .. is refused as an address");

    // find
    core::Json args = core::json_obj({{"query", std::make_shared<core::Json>(core::json_str("quadlet units podman"))}});
    const auto f = core::json_parse(r.find(args));
    const auto* hits = core::json_get(f, "hits");
    chk(hits && !hits->arr.empty(), "find: hits for what the words are about");
    if (hits && !hits->arr.empty()) {
        const auto& top = *hits->arr[0];
        chk(core::json_get(top, "uri") && core::json_get(top, "strength") && core::json_get(top, "read") && core::json_get(top, "matched"), "find: uri, strength, how to read it, why it matched");
    }
    core::Json none = core::json_obj({{"query", std::make_shared<core::Json>(core::json_str("zzzz qqqq"))}});
    const auto nf = core::json_parse(r.find(none));
    chk(core::json_get(nf, "hint") && core::json_get(nf, "hits")->arr.empty(), "find: nothing matched: a hint, not an empty shrug");
    core::Json kinds = core::json_obj({{"query", std::make_shared<core::Json>(core::json_str("quadlet"))},
                                       {"kinds", std::make_shared<core::Json>(core::json_arr({std::make_shared<core::Json>(core::json_str("memory"))}))}});
    const auto mf = core::json_parse(r.find(kinds));
    bool all_memory = true;
    for (const auto& h : core::json_get(mf, "hits")->arr) all_memory = all_memory && core::json_get(*h, "kind")->str == "memory";
    chk(all_memory && !core::json_get(mf, "hits")->arr.empty(), "find: kinds narrows it to memory");
    chk(core::json_get(core::json_parse(r.find(core::json_obj())), "error"), "find: a query is required");

    // skill_load: the old way in
    core::Json sl = core::json_obj({{"name", std::make_shared<core::Json>(core::json_str("git-branch-workflow"))}});
    const auto l = core::json_parse(r.skill_load(sl));
    chk(core::json_get(l, "name")->str == "git-branch-workflow" && has(core::json_get(l, "text")->str, "Rebase before you merge"), "skill_load: still works, the same page");

    // every open is remembered: after a rebase the skill is pointed to again
    s->router->on(turn("something else entirely about nothing in particular"));
    s->router->on(Rebased{1, "q"});
    chk(has(s->router->reattach(), "git-branch-workflow") || has(s->router->reattach(), "podman"), "opens by the model are what the router remembers: " + s->router->reattach());
    chk(core::json_get(Resolver::find_schema(), "parameters") && core::json_get(Resolver::skill_load_schema(), "description"), "schemas for the registry");
}

// ================= the prompt: one voice =================
static void test_prompt() {
    PromptComposer::Contract c;
    const std::string both = PromptComposer::retrieval_contract(c);
    chk(has(both, "<retrieval> notes") && has(both, "MUST be opened") && has(both, "read_file(\"skill://name\")") && has(both, "read_file(\"mem://id\")"), "prompt: the contract names the pointers and how to open them");
    chk(has(both, "NEVER guess a path") && has(both, "find(") && has(both, "run the command"), "prompt: never guess, find first, live state is a command");
    chk(has(both, "mazemaker_remember") && has(both, "already writes every finished turn"), "prompt: save only distilled facts");
    chk(!has(both, "RECALL FIRST") && !has(both, "NO EXCEPTIONS") && !has(both, "session_search") && !has(both, "already done"), "prompt: none of the three old voices");
    chk(both.size() < 1500, "prompt: the contract is short (" + std::to_string(both.size()) + " chars)");
    c.memory = false;
    c.find = false;
    const std::string small = PromptComposer::retrieval_contract(c);
    chk(!has(small, "mem://") && !has(small, "mazemaker_remember") && !has(small, "find("), "prompt: only what exists is promised");
    c.skills = false;
    chk(!has(PromptComposer::retrieval_contract(c), "skill://"), "prompt: no skill catalogue, no skill://");

    chk(PromptComposer::instructions_mode("", true) == PromptComposer::Instructions::Harness, "prompt: the maze's own protocol is replaced by default");
    chk(PromptComposer::instructions_mode("", false) == PromptComposer::Instructions::Verbatim, "prompt: another server's notes stay");
    chk(PromptComposer::instructions_mode("verbatim", true) == PromptComposer::Instructions::Verbatim && PromptComposer::instructions_mode("OFF", false) == PromptComposer::Instructions::Off,
        "prompt: configurable per server");
    chk(PromptComposer::server_instructions(PromptComposer::Instructions::Harness, "RECALL FIRST").empty() &&
            PromptComposer::server_instructions(PromptComposer::Instructions::Off, "x").empty() &&
            PromptComposer::server_instructions(PromptComposer::Instructions::Verbatim, "keep me") == "keep me",
        "prompt: harness and off drop the text, verbatim keeps it");
    PromptComposer pc;
    pc.section("one").section("   ").section("two\n").section("");
    chk(pc.str() == "one\n\ntwo", "prompt: sections in order, empty ones skipped");
    chk(PromptComposer::catalogue_map({"skills 3: a 2", "", "memory 1"}).find("- skills 3: a 2\n- memory 1") != std::string::npos && PromptComposer::catalogue_map({"", ""}).empty(),
        "prompt: the catalogue map lists what exists");
}

static void test_policy() {
    core::Config cfg(core::parse_yaml("knowledge:\n  deep_ms: 900\n  budget:\n    turn_tokens: 400\n    hint_tokens: 200\n  confidence:\n    offer_score: 0.2\n"
                                      "    open_score: 0.4\n  mid_turn:\n    stall: false\nskills:\n  auto_route: false\n"));
    const Policy p = Policy::from(cfg);
    chk(p.deep_ms == 900 && p.turn_tokens == 400 && p.hint_tokens == 200 && p.skills.offer_score == 0.2 && p.skills.open_score == 0.4, "policy: knobs from config.yaml");
    chk(!p.stall && p.shift && p.failure && !p.auto_route, "policy: switches");
    chk(p.failure_tokens > p.hint_tokens && p.stall_tokens > p.failure_tokens, "policy: a failure or a stall may say a little more than a shift");
    const Policy d = Policy::from(core::Config(core::Json{}));
    chk(d.skills.min_matched == 2 && d.skills.offer_mass == 6.5 && d.skills.offer_score == 0.15 && d.skills.open_score == 0.25, "policy: the calibrated defaults");
    Hit h;
    h.matched = 2;
    h.mass = 7.0;
    h.score = 0.2;
    chk(d.skills.offers(h) && !d.skills.opens(h), "policy: offered, not opened at 0.20");
    h.score = 0.3;
    chk(d.skills.opens(h), "policy: opened at 0.30");
    h.matched = 1;
    chk(!d.skills.offers(h), "policy: one matched term is not enough, however well it scores");
    chk(d.skills.needed(568) > 6.49 && d.skills.needed(568) < 6.51, "policy: the calibrated bar at the size it was calibrated at");
    chk(d.skills.needed(20) < 3.5 && d.skills.needed(20) >= 1.0 && d.skills.needed(3700) > 8.0, "policy: the bar follows ln(catalogue size): lower for twenty skills, higher for thousands");
    chk(d.skills.needed(1) >= 1.0 && d.skills.needed(0) == 6.5, "policy: never under 1; size 0 means the absolute bar");
    Hit small;
    small.matched = 2;
    small.mass = 3.3;
    small.score = 0.3;
    chk(!d.skills.offers(small) && d.skills.offers(small, 20), "policy: two good terms in a catalogue of twenty are enough");
    h.matched = 3;
    h.mass = 3.0;
    chk(!d.skills.offers(h), "policy: little evidence is not enough");
}

int main() {
    const fs::path tmp = fs::temp_directory_path() / ("dd_know_router_test_" + std::to_string(::getpid()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    test_records();
    test_working_set();
    test_triggers();
    test_deliverer();
    test_memory_catalogue(tmp);
    test_router_turn(tmp);
    test_router_open_head(tmp);
    test_router_budget(tmp);
    test_router_deep(tmp);
    test_router_midturn(tmp);
    test_router_tools(tmp);
    test_prompt();
    test_policy();

    fs::remove_all(tmp);
    if (g_fail) std::printf("%d FAILED\n", g_fail);
    else std::printf("all know router tests passed\n");
    return g_fail ? 1 : 0;
}
