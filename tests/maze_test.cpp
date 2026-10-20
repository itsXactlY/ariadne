// dd::context × mazemaker — the session as a chain in the graph, the turn
// recall enriched from it, skills from the catalogue, resume from the chain.
// A fake graph stands in for the pod; it is called from the soak worker and
// the recall branches at once, so it holds nothing but atomics and queues.
#include "dd/agent/http_llm.hpp"
#include "dd/context/deck.hpp"
#include "dd/context/endpoint.hpp"
#include "dd/context/engine.hpp"
#include "dd/context/maze.hpp"
#include "dd/context/recall.hpp"
#include "dd/core/mpsc.hpp"
#include "dd/tools/registry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace dd;
using namespace dd::context;

static int g_fail = 0;
static void chk(bool ok, const std::string& label) {
    if (!ok) { ++g_fail; std::printf("FAIL %s\n", label.c_str()); }
}

// Read-only after setup, except the id counter and the write queues.
struct FakeGraph : Memory {
    std::map<long, Hit> rows;                       // id -> row (fixed before threads start)
    std::vector<Hit> recall_hits;
    std::map<long, std::vector<Hit>> activations;   // think()
    std::map<long, std::vector<Hit>> extracted;     // facts()
    std::string intent_of = "general";
    std::atomic<long> next_id{9000};
    core::MpscQueue<Hit> remembered;
    core::MpscQueue<Edge> edges;
    std::atomic<int> recalls{0};

    bool configured() const override { return true; }
    bool up() const override { return true; }
    bool probe() override { return true; }
    std::vector<Hit> recall(const std::string&, int, const std::string&) override {
        ++recalls;
        return recall_hits;
    }
    long remember(const std::string& label, const std::string& content) override {
        const long id = next_id.fetch_add(1);
        remembered.push(Hit{id, label, content, 0});
        return id;
    }
    std::vector<Hit> think(long id, int) override {
        auto it = activations.find(id);
        return it == activations.end() ? std::vector<Hit>{} : it->second;
    }
    std::vector<Hit> get(const std::vector<long>& ids) override {
        std::vector<Hit> out;
        for (long id : ids)
            if (auto it = rows.find(id); it != rows.end()) out.push_back(it->second);
        return out;
    }
    std::vector<Hit> facts(long id, int) override {
        auto it = extracted.find(id);
        return it == extracted.end() ? std::vector<Hit>{} : it->second;
    }
    std::string intent(const std::string&) override { return intent_of; }
    std::vector<Hit> labels(const std::string& prefix, int offset, int limit) override {
        std::vector<Hit> out;
        int skip = offset;
        for (const auto& [id, h] : rows)
            if (h.label.rfind(prefix, 0) == 0 && skip-- <= 0 && static_cast<int>(out.size()) < limit) out.push_back(Hit{id, h.label, "", 0});
        return out;
    }
    int connect(const std::vector<Edge>& e) override {
        for (const auto& x : e) edges.push(x);
        return static_cast<int>(e.size());
    }
};

static void wait_written(Soaker& s, uint64_t n) {
    for (int i = 0; i < 200 && s.stats().written < n; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

int main() {
    // ---- the soaker writes a chain: ids back, edges between nodes, labels unique ----
    {
        auto g = std::make_shared<FakeGraph>();
        Soaker s(g, "");
        s.reasoning("dd-x", "thinking about it", 0);
        s.turn("dd-x", "q1", "a1", 0);
        s.turn("dd-x", "q2", "a2", 0);   // same second as q1: the label moves on
        wait_written(s, 3);
        for (int i = 0; i < 100 && s.stats().edges < 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::vector<LedgerEntry> led;
        LedgerEntry e;
        while (s.take(e)) led.push_back(e);
        chk(led.size() == 3 && led[0].kind == "reasoning" && led[1].kind == "turn" && led[2].kind == "turn" &&
                led[1].id > 0 && led[1].label != led[2].label, "ledger: ids back in order, labels unique");
        std::vector<Edge> ed;
        Edge x;
        while (g->edges.pop(x)) ed.push_back(x);
        chk(ed.size() == 2, "two edges");
        bool reasoned = false, next = false;
        for (const auto& d : ed) {
            reasoned = reasoned || (d.type == "reasoned_in" && d.a == led[0].label && d.b == led[1].label);
            next = next || (d.type == "session_next" && d.a == led[1].label && d.b == led[2].label);
        }
        chk(reasoned && next, "reasoning -> its turn, turn -> next turn");
        s.continue_chain("auto:turn:dd-x:abc");
        s.turn("dd-x", "q3", "a3", 1);
        for (int i = 0; i < 100 && s.stats().edges < 3; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        bool chained = false;
        while (g->edges.pop(x)) chained = chained || (x.a == "auto:turn:dd-x:abc" && x.type == "session_next");
        chk(chained, "a resumed chain continues from its last turn");
    }

    // ---- the recaller: one recall, the graph around it, nothing twice ----
    {
        auto g = std::make_shared<FakeGraph>();
        g->recall_hits = {Hit{1, "decision:tape", "the tape renders once", 0.7},
                          Hit{2, "auto:turn:dd-x:1", "this turn is on the tape", 0.6},
                          Hit{3, "fact:kv", "llama reuses only a prefix", 0.5}};
        g->rows[10] = Hit{10, "invariant:prefix", "hygiene only at a rebase", 0};
        g->rows[11] = Hit{11, "skill:local:x", "a skill row", 0};
        g->rows[20] = Hit{20, "auto:turn:dd-x:0", "session:dd-x\n\n=== USER ===\nearlier question", 0};
        g->activations[1] = {Hit{2, "auto:turn:dd-x:1", "", 0.4}, Hit{11, "skill:local:x", "", 0.3}, Hit{10, "invariant:prefix", "", 0.2}};
        g->extracted[1] = {Hit{30, "afe", "tape freezes bytes", 0.9}};
        SkillIndex skills(g);
        skills.load_rows({Hit{50, "skill:local:context-tape-debugging",
                              "Skill: context-tape-debugging\nCategory: daedalus\n\nDebug prefix breaks in the context tape and KV reuse.\n\nFull text in mazemaker: …", 0}});
        Recaller r(g, &skills);
        r.skill_min = 0.1;   // a one-skill corpus scores low; the real cutoff is measured on 503
        const Prefetch p = r.run("why does the context tape break the KV prefix", {2}, {20});
        chk(p.dropped == 1 && p.hits == 2, "a hit already in the window is dropped");
        chk(p.linked == 1 && p.block.find("hygiene only at a rebase") != std::string::npos, "the graph adds a neighbour by id");
        chk(p.block.find("skill:local:x") == std::string::npos, "skill rows are not neighbours");
        chk(p.facts == 1 && p.block.find("tape freezes bytes") != std::string::npos, "facts of the top hit");
        chk(p.earlier == 0, "earlier turns only for a temporal question");
        chk(p.skill == "context-tape-debugging" && p.block.find("skill_load(\"context-tape-debugging\")") != std::string::npos,
            "a fitting skill, matched locally");
        chk(g->recalls.load() == 1, "one recall per turn");
        chk(p.block.find("- #1 [decision:tape]") != std::string::npos && p.block.find("#10 [invariant:prefix]") != std::string::npos,
            "every row names its id for mazemaker_get");
        chk(p.shown.size() == 4 && p.shown[0].first == 'r' && p.shown[0].second.id == 1 && p.shown[2].first == 'l' && p.shown[3].first == 'f',
            "the card gets the rows the block carries");
        g->intent_of = "temporal";
        const Prefetch t = r.run("what did we do earlier", {2}, {20});
        chk(t.earlier == 1 && t.block.find("earlier question") != std::string::npos, "temporal: this session's turns by id");
        const Prefetch small = r.run("x", {}, {}, 120);
        chk(small.block.size() < 600, "the budget holds");
    }

    // ---- skills: catalogue in the maze, parts reassembled in order ----
    {
        auto g = std::make_shared<FakeGraph>();
        g->rows[60] = Hit{60, "skillsrc:local:alpha#001", "second half", 0};
        g->rows[61] = Hit{61, "skillsrc:local:alpha#000", "first half, ", 0};
        SkillIndex s(g);
        s.load_rows({Hit{70, "skill:builtin:alpha", "Skill: alpha\n\nbuiltin copy", 0},
                     Hit{71, "skill:local:alpha", "Skill: alpha\n\nthe operator's copy", 0},
                     Hit{72, "skill:local:beta", "Skill: beta\n\ndescription: \"brews coffee\"\n", 0}});
        chk(s.size() == 2, "one skill per name");
        std::string name;
        chk(s.load("alpha", &name) == "first half, second half" && name == "alpha", "parts joined in label order");
        chk(s.search("coffee", 1).size() == 1 && s.search("coffee", 1)[0].first.name == "beta", "description searched");
        chk(!s.match("list_dir /srv/alpha/beta/coffee and say what is there", 0.1), "paths do not pull skills");
    }

    // ---- the engine: rebase names ids; resume rebuilds the session from its chain ----
    {
        auto g = std::make_shared<FakeGraph>();
        g->rows[100] = Hit{100, "auto:turn:dd-old:6ad70001", "session:dd-old @ x\n\n=== USER ===\nfirst q\n\n=== ASSISTANT ===\nfirst a", 0};
        g->rows[101] = Hit{101, "auto:turn:dd-old:6ad70002", "session:dd-old @ x\n\n=== USER ===\nsecond q\n\n=== ASSISTANT ===\nsecond a", 0};
        g->recall_hits = {Hit{100, "auto:turn:dd-old:6ad70001", "first q / first a", 0.6}};
        tools::ToolRegistry reg;
        ToolDeck deck(reg, tools::ToolSearchConfig::from_raw(core::Json{}));
        deck.arrange({});
        Endpoint ep("http://203.0.113.1/v1");
        agent::HttpLLM llm;
        EngineConfig cfg;
        cfg.fast_layer_tokens = 100000;
        cfg.prewarm = false;
        Soaker soaker(g, "");
        TapeEngine eng(cfg, "sys", ep, g, &soaker, deck, llm, std::filesystem::temp_directory_path().string());
        const auto recent = eng.recent_sessions("dd-", 5);
        chk(recent.size() == 1 && recent[0].id == "dd-old" && recent[0].turns == 2, "sessions listed from the maze");
        std::string why;
        chk(eng.resume("dd-old", &why), "resume: " + why);
        const auto& items = eng.session().items();
        chk(items.size() == 4 && items[0]->msg.content == "first q" && items[3]->msg.content == "second a" &&
                eng.session().id() == "dd-old", "the chain becomes the session");
        eng.begin_turn("and now the third question about the earlier answers, please");
        const auto p = eng.payload();
        bool dropped_own = true;   // the resumed turns are on the tape: the recall must not inject them again
        for (const auto& m : p)
            if (m.role == "user" && m.content.find("<memory-context>") != std::string::npos)
                dropped_own = m.content.find("first q / first a") == std::string::npos;
        chk(dropped_own, "a resumed turn is not recalled into the window it is already in");
        eng.rebase_now();
        const auto& base = eng.session().items();
        chk(base.size() >= 2 && base[1]->msg.content.find("memory ids [100,101]") != std::string::npos &&
                base[1]->msg.content.find("mazemaker_get(memory_ids=[100,101])") != std::string::npos,
            "the rebase anchor names the turns by id");
    }

    if (g_fail) std::printf("%d FAILED\n", g_fail);
    else std::printf("all maze tests passed\n");
    return g_fail ? 1 : 0;
}
