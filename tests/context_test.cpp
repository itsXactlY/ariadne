// dd::context — the tape keeps every request an exact prefix of the next;
// hygiene, rebase, tool deck and soak behave as the Python harness does.
#include "dd/agent/http_llm.hpp"
#include "dd/context/deck.hpp"
#include "dd/context/endpoint.hpp"
#include "dd/context/engine.hpp"
#include "dd/context/maze.hpp"
#include "dd/context/render.hpp"
#include "dd/context/tape.hpp"
#include "dd/tools/registry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace dd;
using namespace dd::context;
using core::Json;
using core::Message;

static int g_fail = 0;
static void chk(bool ok, const std::string& label) {
    if (!ok) { ++g_fail; std::printf("FAIL %s\n", label.c_str()); }
}

static core::JsonSP jsp(Json v) { return std::make_shared<Json>(std::move(v)); }

static std::vector<std::string> wire(const std::vector<Message>& msgs) {
    std::vector<std::string> out;
    for (const auto& m : msgs) out.push_back(core::json_dump(agent::wire_message(m)));
    return out;
}

static bool is_prefix(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    if (a.size() > b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) return false;
    return true;
}

static Message assistant_calls(int n, const std::string& tool, const std::string& path, const std::string& reasoning) {
    Message m;
    m.role = "assistant";
    m.reasoning = reasoning;
    m.content = "";
    core::ToolCall c;
    c.id = "call_" + std::to_string(n);
    c.name = tool;
    c.arguments = "{\"path\":\"" + path + "\"}";
    m.tool_calls.push_back(c);
    return m;
}

static Message tool_result(int n, const std::string& content) {
    Message m;
    m.role = "tool";
    m.tool_call_id = "call_" + std::to_string(n);
    m.content = content;
    return m;
}

static void add_tool(tools::ToolRegistry& reg, const std::string& name, const std::string& toolset, bool requires_path) {
    tools::ToolEntry e;
    e.name = name;
    e.toolset = toolset;
    Json props = core::json_obj({{"path", jsp(core::json_obj({{"type", jsp(core::json_str("string"))}}))}});
    Json params = core::json_obj({{"type", jsp(core::json_str("object"))}, {"properties", jsp(props)}});
    if (requires_path) core::obj_set(params, "required", core::json_arr({jsp(core::json_str("path"))}));
    e.schema = core::json_obj({{"description", jsp(core::json_str(name + " does things with " + toolset + " memory graphs"))},
                               {"parameters", jsp(params)}});
    e.handler = [name](const Json& a) {
        const Json* p = core::json_get(a, "path");
        return std::string("{\"tool\":\"") + name + "\",\"path\":\"" + (p ? p->str : "") + "\"}";
    };
    reg.register_tool(e);
}

// A tool-heavy session against the engine, offline (remote endpoint, no maze).
// Returns how many requests were NOT an exact prefix-extension of the previous one.
static int prefix_breaks(bool tape, int* calls_out, std::vector<std::vector<std::string>>* payloads = nullptr) {
    tools::ToolRegistry reg;
    add_tool(reg, "read_file", "", true);
    ToolDeck deck(reg, tools::ToolSearchConfig::from_raw(core::json_obj({{"enabled", jsp(core::json_str("off"))}})));
    deck.arrange({"read_file"});
    Endpoint ep("http://203.0.113.1/v1");   // remote: no probes, no slots
    auto maze = std::make_shared<Maze>("");
    agent::HttpLLM llm;
    EngineConfig cfg;
    cfg.tape = tape;
    cfg.render.window_turns = 3;      // a small head so the window moves turn to turn
    cfg.render.tool_group_ttl = 2;    // groups age out quickly
    cfg.render.reasoning_window = 1;
    cfg.turn_recall = false;
    cfg.prewarm = false;
    const std::string spill = std::filesystem::temp_directory_path().string() + "/dd_context_test_spill";
    TapeEngine eng(cfg, "you are a test", ep, maze, nullptr, deck, llm, spill);

    int breaks = 0, calls = 0, n = 0;
    std::vector<std::string> prev;
    auto call = [&] {
        const auto p = wire(eng.payload());
        if (!prev.empty() && !is_prefix(prev, p)) ++breaks;
        if (payloads) payloads->push_back(p);
        prev = p;
        ++calls;
    };
    for (int turn = 0; turn < 5; ++turn) {
        eng.begin_turn("turn " + std::to_string(turn) + ": read the files and report what each holds");
        for (int k = 0; k < 3; ++k) {
            call();
            ++n;
            eng.on_assistant(assistant_calls(n, "read_file", "/f" + std::to_string(n),
                                             std::string(1200, static_cast<char>('a' + n % 26))));
            eng.on_tool_results({tool_result(n, "content of /f" + std::to_string(n) + " " + std::string(900, 'x'))});
            eng.after_tools();
        }
        call();
        Message fin;
        fin.role = "assistant";
        fin.content = "turn " + std::to_string(turn) + " done";
        fin.reasoning = std::string(1500, 'r');
        eng.on_assistant(fin);
        eng.end_turn(fin.content, false);
    }
    if (calls_out) *calls_out = calls;
    return breaks;
}

int main() {
    // ---- tape ----
    {
        Session s("dd-test");
        Message m;
        m.role = "user";
        m.content = "q";
        auto a = s.append(m), b = s.append(m);
        Tape t;
        t.start({a}, {m}, s.generation());
        t.append(b, {m});
        auto r = t.matched({a, b}, s.generation());
        chk(r.first == 2 && r.second == 2, "tape matches by identity");
        auto copy = std::make_shared<Item>(*a);
        chk(t.matched({copy, b}, s.generation()).first == 0, "a copy is another history");
        chk(t.matched({a, b}, s.generation() + 1).first == 0, "another generation is another epoch");
        chk(s.current_user() == 1, "current user");
    }

    // ---- the invariant: 0 prefix breaks with the tape, many without ----
    {
        int calls = 0;
        std::vector<std::vector<std::string>> payloads;
        const int with = prefix_breaks(true, &calls, &payloads);
        const int without = prefix_breaks(false, nullptr);
        std::printf("prefix breaks over %d calls: tape %d, per-call rendering %d\n", calls, with, without);
        chk(with == 0, "tape: every request extends the previous one");
        chk(without > 0, "without the tape the window and TTLs rewrite the middle");
        // nothing rewritten mid-epoch: the oldest groups never collapse while the tape runs
        bool aged = false;
        for (const auto& w : payloads.back()) aged = aged || w.find("[aged out of context]") != std::string::npos;
        chk(!aged, "mid-epoch payloads keep what they first sent");
    }

    // ---- renderer: epoch hygiene ----
    {
        std::vector<std::string> soaked;
        RenderPolicy pol;
        pol.tool_group_ttl = 1;
        pol.reasoning_window = 1;
        bool maze_up = true;
        Spill spill(std::filesystem::temp_directory_path().string() + "/dd_context_test_render");
        Renderer r(pol, [&] { return maze_up; }, [&](Item& it) { soaked.push_back(it.msg.reasoning); it.soaked = true; }, &spill);
        Session s("dd-r");
        Message u;
        u.role = "user";
        u.content = "question";
        s.append(u);
        s.append(assistant_calls(1, "read_file", "/old.txt", std::string(1500, 'o')));
        s.append(tool_result(1, "old"));
        s.append(assistant_calls(2, "read_file", "/new.txt", std::string(1500, 'n')));
        s.append(tool_result(2, "new"));
        auto ep = r.epoch(s.items(), 0, "<memory-context>x</memory-context>");
        const auto w = wire(ep.rendered);
        chk(w.size() == 4, "one group collapsed to one line");
        chk(w[0].find("question\\n\\n<memory-context>") != std::string::npos, "recall rides the question");
        chk(w[1].find("[aged out of context] read_file(/old.txt)") != std::string::npos, "collapsed line names the call");
        chk(w[2].find("reasoning_content") != std::string::npos, "newest reasoning kept");
        chk(soaked.size() == 2, "collapsed and current-turn thinking soaked once each");
        // maze down: aged reasoning goes to disk behind a handle
        maze_up = false;
        pol.tool_group_ttl = 8;
        Renderer r2(pol, [&] { return maze_up; }, nullptr, &spill);
        const auto w2 = wire(r2.epoch(s.items(), 0, "").rendered);
        chk(w2[1].find("read_file(\\\"") != std::string::npos && w2[1].find("earlier reasoning") != std::string::npos,
            "dead maze: reasoning spilled behind a read_file handle");
    }
    {   // capped results
        RenderPolicy pol;
        pol.context_length = 20000;   // 10% share -> 8000 chars
        Renderer r(pol, [] { return true; }, nullptr, nullptr);
        Session s("dd-c");
        s.append(assistant_calls(7, "run_terminal", "/x", ""));
        Message big = tool_result(7, std::string(30000, 'z'));
        auto it = s.append(big);
        const auto out = r.one(*it, false, "", s.items());
        chk(out[0].content.size() < 9000 && out[0].content.find(Renderer::kResultCut) != std::string::npos &&
                out[0].content.find("run a narrower call") != std::string::npos, "an oversized result is cut with a marker");
    }
    {   // sanitize
        Message orphan = tool_result(99, "x");
        Message calls = assistant_calls(5, "a", "/p", "");
        auto out = Renderer::sanitize({orphan, calls});
        chk(out.size() == 2 && out[0].role == "assistant" && out[1].role == "tool" && out[1].tool_call_id == "call_5",
            "orphan result dropped, missing result stubbed");
        core::ToolCall c;
        c.name = "patch";
        c.arguments = "{\"file_path\":\"src/world.cpp\",\"x\":1}";
        chk(Renderer::describe_call(c) == "patch(src/world.cpp)", "describe_call");
        c.arguments = "{}";
        chk(Renderer::describe_call(c) == "patch()", "describe_call empty");
    }

    // ---- rebase ----
    {
        tools::ToolRegistry reg;
        add_tool(reg, "read_file", "", true);
        ToolDeck deck(reg, tools::ToolSearchConfig::from_raw(core::json_obj({{"enabled", jsp(core::json_str("off"))}})));
        deck.arrange({"read_file"});
        Endpoint ep("http://203.0.113.1/v1");
        auto maze = std::make_shared<Maze>("");
        agent::HttpLLM llm;
        EngineConfig cfg;
        cfg.fast_layer_tokens = 10000;   // hard = 9000
        cfg.protect_last_n = 2;
        cfg.turn_recall = false;
        cfg.prewarm = false;
        TapeEngine eng(cfg, "sys", ep, maze, nullptr, deck, llm, std::filesystem::temp_directory_path().string());
        std::vector<std::string> notes;
        eng.on_note = [&](const std::string& s) { notes.push_back(s); };
        eng.begin_turn("first question, long enough to anchor a short follow-up later on");
        eng.payload();
        for (int i = 1; i <= 4; ++i) {
            eng.on_assistant(assistant_calls(i, "read_file", "/r" + std::to_string(i), ""));
            eng.on_tool_results({tool_result(i, "r" + std::to_string(i))});
            agent::CallStats st;
            st.prompt_tokens = 2000 * i;
            st.completion_tokens = 100;
            eng.on_call(st);
            eng.after_tools();
        }
        // the 4th call reported 8100 tokens: under hard. One more pushes it over, inside the turn.
        agent::CallStats st;
        st.prompt_tokens = 9500;
        eng.on_call(st);
        eng.on_assistant(assistant_calls(5, "read_file", "/r5", ""));
        eng.on_tool_results({tool_result(5, "r5")});
        eng.after_tools();
        const auto& items = eng.session().items();
        chk(eng.session().generation() == 1, "hard watermark inside the turn: one rebase");
        chk(items.size() >= 3 && items[0]->msg.content.rfind("first question", 0) == 0, "the question leads turn 0");
        chk(items[1]->hidden && items[1]->msg.content.find("[rebased]") != std::string::npos &&
                items[1]->msg.content.find("auto:turn:" + eng.session().id()) != std::string::npos,
            "anchor names where the rest is");
        chk(items[1]->msg.content.find("[this turn so far]") != std::string::npos &&
                items[1]->msg.content.find("read_file(/r1)") != std::string::npos,
            "what the turn already ran survives as one line per call");
        chk(items[1]->msg.content.find("[memory unreachable]") != std::string::npos, "a dead maze is said, not hidden");
        chk(items.back()->msg.role == "tool" && items.back()->msg.content == "r5", "the protected tail stays");
        const auto p = wire(eng.payload());
        chk(p.size() == 1 + items.size(), "a new epoch renders the rebased history");
        chk(!notes.empty() && notes.back().find("rebase (inside the turn)") != std::string::npos, "rebase announced");
    }

    // ---- tool deck ----
    {
        tools::ToolRegistry reg;
        add_tool(reg, "read_file", "", true);
        add_tool(reg, "mazemaker_recall", "mazemaker", false);
        add_tool(reg, "mazemaker_browse", "mazemaker", false);
        add_tool(reg, "mazemaker_graph", "mazemaker", true);
        auto cfg = tools::ToolSearchConfig::from_raw(core::json_obj(
            {{"enabled", jsp(core::json_bool(true))},
             {"always_load", jsp(core::json_arr({jsp(core::json_str("mazemaker_recall"))}))}}));
        ToolDeck deck(reg, cfg);
        deck.arrange({"read_file"});
        const Json defs = deck.definitions();
        std::vector<std::string> names;
        for (const auto& d : defs.arr)
            if (const Json* f = core::json_get(*d, "function")) names.push_back(core::json_get(*f, "name")->str);
        chk(deck.active() && deck.deferred() == 2 && names.size() == 5, "2 visible + 3 bridges, 2 deferred");
        chk(std::find(names.begin(), names.end(), "mazemaker_browse") == names.end(), "deferred tools stay out of the array");
        const std::string hit = reg.dispatch("tool_search", core::json_obj({{"query", jsp(core::json_str("graph"))}}));
        chk(hit.find("mazemaker_graph") != std::string::npos, "tool_search finds a deferred tool");
        const std::string schema = reg.dispatch("tool_describe", core::json_obj({{"name", jsp(core::json_str("mazemaker_graph"))}}));
        chk(schema.find("\"parameters\"") != std::string::npos, "tool_describe returns the schema");
        const std::string missing = reg.dispatch("tool_call", core::json_obj({{"name", jsp(core::json_str("mazemaker_graph"))}}));
        chk(missing.find("missing required") != std::string::npos, "tool_call checks required arguments");
        const std::string ran = reg.dispatch("tool_call", core::json_obj(
            {{"name", jsp(core::json_str("mazemaker_graph"))},
             {"arguments", jsp(core::json_str("{\"path\":\"/g\"}"))}}));
        chk(ran.find("\"tool\":\"mazemaker_graph\"") != std::string::npos && ran.find("/g") != std::string::npos,
            "tool_call runs the deferred tool");
        chk(reg.dispatch("tool_call", core::json_obj({{"name", jsp(core::json_str("read_file"))}})).find("not a deferrable") !=
                std::string::npos, "a visible tool is called directly");
    }

    // ---- soak ----
    {
        chk(Soaker::label("turn", "dd-1", 0x1f) == "auto:turn:dd-1:1f", "turn label");
        chk(Soaker::label("reasoning", "s", 255) == "auto:reasoning:s:ff", "reasoning label");
        const std::string c = Soaker::turn_content("s1", "hi there", "<think>secret</think>the answer", 0);
        chk(c == "session:s1 @ 1970-01-01T00:00:00Z\n\n=== USER ===\nhi there\n\n=== ASSISTANT ===\nthe answer",
            "turn content, thinking scrubbed");
        chk(Soaker::scrub_think("a<think>b") == "a", "an unclosed think cuts the rest");
        chk(Soaker::non_answer("Operation interrupted: waiting") && Soaker::non_answer("[Request interrupted by user]") &&
                Soaker::non_answer("  ") && !Soaker::non_answer("No response was needed here, so I moved on"),
            "non-answers");
        chk(Soaker::reasoning_content("s", std::string(4000, 'q'), 0).find("…[truncated]") != std::string::npos,
            "long reasoning is cut");
        const std::string spool = std::filesystem::temp_directory_path().string() + "/dd_context_test_spool.jsonl";
        std::remove(spool.c_str());
        {
            Soaker s(std::make_shared<Maze>("http://127.0.0.1:9/mcp"), spool);   // nothing listens on :9
            s.turn("s", "q", "an answer");
            s.turn("s", "q", "Operation interrupted");
            for (int i = 0; i < 100 && s.stats().spooled + s.stats().written < 1; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const auto st = s.stats();
            chk(st.queued == 1 && st.skipped == 1 && st.spooled == 1, "dead maze: the turn is spooled, the non-answer skipped");
        }
        std::ifstream in(spool);
        std::string line;
        std::getline(in, line);
        chk(line.find("auto:turn:s:") != std::string::npos && line.find("an answer") != std::string::npos, "spool line");
        std::remove(spool.c_str());
    }

    // ---- helpers ----
    {
        chk(trivial_prompt("ok!") && trivial_prompt("/help") && trivial_prompt("Thank you.") && !trivial_prompt("ok, now fix the bug"),
            "trivial prompts");
        Session s("x");
        Message u;
        u.role = "user";
        u.content = "please port the whole payload tape to the c++ harness";
        s.append(u);
        u.content = "weiter";
        s.append(u);
        chk(anchored_query(s.items(), 1) == "please port the whole payload tape to the c++ harness\nweiter",
            "a short follow-up is anchored on the last real question");
    }

    if (g_fail) std::printf("%d FAILED\n", g_fail);
    else std::printf("all context tests passed\n");
    return g_fail ? 1 : 0;
}
