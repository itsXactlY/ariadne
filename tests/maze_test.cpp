// dd::context × mazemaker — the session as a chain in the graph, the turn
// recall enriched from it, resume from the chain (the skill catalogue: know_test.cpp).
// A fake graph stands in for the pod; it is called from the soak worker and
// the recall branches at once, so it holds nothing but atomics and queues.
#include "dd/agent/http_llm.hpp"
#include "dd/context/deck.hpp"
#include "dd/context/endpoint.hpp"
#include "dd/context/engine.hpp"
#include "dd/context/fsutil.hpp"
#include "dd/context/maze.hpp"
#include "dd/context/recall.hpp"
#include "dd/core/json.hpp"
#include "dd/core/mpsc.hpp"
#include "dd/tools/mcp.hpp"
#include "dd/tools/registry.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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


// Scratch lives under $DD_TEST_TMP (the Makefile sets it), never in the shared /tmp; one directory per process, gone at exit.
static std::string tmp_root_s() {
    static const std::string s = [] {
        const char* e = std::getenv("DD_TEST_TMP");
        return (std::filesystem::path(e && *e ? e : "build/tmp") / ("maze-" + std::to_string(::getpid()))).string();
    }();
    return s;
}
static std::string tmp_dir(const std::string& name) {
    const auto p = std::filesystem::path(tmp_root_s()) / name;
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return p.string();
}
static struct TmpCleanup {
    std::string dir = tmp_root_s();
    ~TmpCleanup() {
        // destructors run at exit: a throw here would call std::terminate
        try {
            std::error_code ec;
            std::filesystem::remove_all(std::filesystem::path(dir), ec);
        } catch (...) {}
    }
} g_tmp_cleanup;

static unsigned mode_of(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 ? (st.st_mode & 07777u) : 0xFFFFu;
}

static size_t lines_in(const std::string& path) {
    std::ifstream in(path);
    size_t n = 0;
    for (std::string l; std::getline(in, l);) ++n;
    return n;
}

// A loopback MCP pod: every POST answers one tools/call result; GET /health says the maze is reachable.
struct FakePod {
    int lfd = -1, port = 0;
    std::atomic<bool> stop{false};
    std::thread th;
    std::string text = "";
    bool is_error = false, rpc_error = false;

    FakePod() {
        lfd = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        ::bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof a);
        socklen_t len = sizeof a;
        ::getsockname(lfd, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
        ::listen(lfd, 8);
        th = std::thread([this] { serve(); });
    }
    ~FakePod() { shutdown(); }
    void shutdown() {
        if (lfd < 0) return;
        stop = true;
        const int c = ::socket(AF_INET, SOCK_STREAM, 0);   // wake accept()
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(static_cast<uint16_t>(port));
        ::connect(c, reinterpret_cast<sockaddr*>(&a), sizeof a);
        ::close(c);
        th.join();
        ::close(lfd);
        lfd = -1;
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port) + "/mcp"; }

    void serve() {
        while (!stop) {
            const int c = ::accept(lfd, nullptr, nullptr);
            if (c < 0) continue;
            if (stop) {
                ::close(c);
                break;
            }
            std::string req;
            char buf[4096];
            size_t need = std::string::npos;
            for (;;) {
                const ssize_t n = ::recv(c, buf, sizeof buf, 0);
                if (n <= 0) break;
                req.append(buf, static_cast<size_t>(n));
                const size_t h = req.find("\r\n\r\n");
                if (h == std::string::npos) continue;
                if (need == std::string::npos) {
                    need = h + 4;
                    const size_t cl = req.find("Content-Length:");
                    const size_t cl2 = req.find("content-length:");
                    const size_t at = cl != std::string::npos ? cl : cl2;
                    if (at != std::string::npos) need += static_cast<size_t>(std::atol(req.c_str() + at + 15));
                }
                if (req.size() >= need) break;
            }
            std::string body, type = "application/json";
            if (req.rfind("GET ", 0) == 0) {
                body = "{\"mazemaker_reachable\":true}";
            } else {
                core::Json id = core::json_num(1);
                try {
                    const core::Json rq = core::json_parse(req.substr(req.find("\r\n\r\n") + 4));
                    if (const core::Json* i = core::json_get(rq, "id")) id = *i;
                } catch (...) {
                }
                core::Json msg = core::json_obj({{"jsonrpc", std::make_shared<core::Json>(core::json_str("2.0"))},
                                                 {"id", std::make_shared<core::Json>(id)}});
                if (rpc_error) {
                    core::obj_set(msg, "error", core::json_obj({{"code", std::make_shared<core::Json>(core::json_num(-32000))},
                                                                {"message", std::make_shared<core::Json>(core::json_str("boom"))}}));
                } else {
                    core::Json part = core::json_obj({{"type", std::make_shared<core::Json>(core::json_str("text"))},
                                                      {"text", std::make_shared<core::Json>(core::json_str(text))}});
                    core::Json res = core::json_obj({{"content", std::make_shared<core::Json>(core::json_arr({std::make_shared<core::Json>(part)}))}});
                    if (is_error) core::obj_set(res, "isError", core::json_bool(true));
                    core::obj_set(msg, "result", res);
                }
                body = "event: message\ndata: " + core::json_dump(msg) + "\n\n";
                type = "text/event-stream";
            }
            const std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) +
                                     "\r\nConnection: close\r\n\r\n" + body;
            size_t off = 0;
            while (off < resp.size()) {
                const ssize_t w = ::send(c, resp.data() + off, resp.size() - off, MSG_NOSIGNAL);
                if (w <= 0) break;
                off += static_cast<size_t>(w);
            }
            ::close(c);
        }
    }
};

// remember() fails until healthy; then it behaves; slow_ms sleeps inside the write.
struct FlakyGraph : FakeGraph {
    std::atomic<bool> healthy{false};
    std::atomic<int> slow_ms{0}, started{0}, finished{0};
    long remember(const std::string& label, const std::string& content) override {
        started.fetch_add(1);
        if (slow_ms.load()) std::this_thread::sleep_for(std::chrono::milliseconds(slow_ms.load()));
        const long id = healthy.load() ? FakeGraph::remember(label, content) : -1;
        finished.fetch_add(1);
        return id;
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
        Recaller r(g);
        const Prefetch p = r.run("why does the context tape break the KV prefix", {2}, {20});
        chk(p.dropped == 1 && p.hits == 2, "a hit already in the window is dropped");
        chk(p.linked == 1 && p.block.find("hygiene only at a rebase") != std::string::npos, "the graph adds a neighbour by id");
        chk(p.block.find("skill:local:x") == std::string::npos, "skill rows are not neighbours");
        chk(p.facts == 1 && p.block.find("tape freezes bytes") != std::string::npos, "facts of the top hit");
        chk(p.earlier == 0, "earlier turns only for a temporal question");
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
        TapeEngine eng(cfg, "sys", ep, g, &soaker, deck, llm, tmp_dir("spill"));
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


    // ---- the spool: private, replayed from the front, one writer at a time ----
    {
        namespace fs = std::filesystem;
        const std::string dir = tmp_dir("spool_case") + "/state/deeper";   // not there yet: the spool makes it
        const std::string spool = dir + "/soak-spool.jsonl";
        auto g = std::make_shared<FlakyGraph>();
        Soaker s(g, spool);
        auto wait_spooled = [&](uint64_t n) {
            for (int i = 0; i < 300 && s.stats().spooled < n; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        };
        s.turn("s", "q1", "a1", 0);
        wait_spooled(1);
        s.turn("s", "q2", "a2", 0);
        wait_spooled(2);
        chk(lines_in(spool) == 2, "spool: a dead pod's turns land on disk");
        chk(mode_of(spool) == 0600 && mode_of(dir) == 0700 && mode_of(dir + "/..") == 0700, "spool: file 0600, the directories it made 0700");
        chk(mode_of(spool + ".lock") == 0600, "spool: the lock file is private too");

        // another dd on this home holds the lock: an append waits for it
        std::atomic<bool> held{false};
        std::thread other([&] {
            const fsx::FileLock lk(spool + ".lock");
            held = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
        });
        while (!held) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const auto t0 = std::chrono::steady_clock::now();
        s.turn("s", "q3", "a3", 0);
        wait_spooled(3);
        const auto waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        other.join();
        chk(s.stats().spooled == 3 && waited >= 300, "spool: an append waits for another process's lock (" + std::to_string(static_cast<int>(waited)) + " ms)");
        chk(lines_in(spool) == 3, "spool: nothing torn or lost across the lock");

        // the pod answers again: the head is replayed and the rest rewritten, still 0600
        fs::permissions(spool, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read);
        g->healthy = true;
        s.turn("s", "q4", "a4", 0);
        for (int i = 0; i < 300 && lines_in(spool) != 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        chk(lines_in(spool) == 2, "spool: one spooled turn replays per live write (" + std::to_string(lines_in(spool)) + " left)");
        chk(mode_of(spool) == 0600, "spool: the rewrite is 0600 (" + std::to_string(mode_of(spool)) + ")");
        Hit first;
        bool saw = false;
        while (g->remembered.pop(first)) saw = saw || first.content.find("q1") != std::string::npos;
        chk(saw, "spool: the oldest turn went first");
        s.turn("s", "q5", "a5", 0);
        s.turn("s", "q6", "a6", 0);
        for (int i = 0; i < 300 && fs::exists(spool); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        chk(!fs::exists(spool), "spool: an emptied spool is removed");
    }

    // ---- the destructor waits for a write in flight, by join, and no longer ----
    {
        auto g = std::make_shared<FlakyGraph>();
        g->healthy = true;
        g->slow_ms = 300;
        {
            const auto t0 = std::chrono::steady_clock::now();
            { Soaker idle(g, ""); }
            const double idle_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            chk(idle_ms < 200, "soaker: nothing in flight, the destructor returns at once (" + std::to_string(static_cast<int>(idle_ms)) + " ms)");
        }
        const auto t0 = std::chrono::steady_clock::now();
        {
            Soaker s(g, "");
            s.turn("s", "q", "an answer", 0);
            while (g->started.load() < 1) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        chk(g->finished.load() == 1 && ms >= 200 && ms < 2500, "soaker: the destructor waits for the write in flight (" + std::to_string(static_cast<int>(ms)) + " ms)");
    }

    // ---- Maze::call: an answered call keeps the maze up, an unreachable pod takes it down ----
    {
        FakePod pod;   // answers every call with an empty text
        Maze m(pod.url());
        chk(m.probe() && m.up(), "maze: /health says it is up");
        m.recall("a question that finds nothing", 3);
        chk(m.up(), "maze: an empty reply that was answered is still an answer");
        pod.text = "[]";
        m.recall("a question with an empty list", 3);
        chk(m.up(), "maze: a reply with content keeps it up");
        // the contract on tool errors, checked only when the MCP client reports them (the client is another module)
        pod.is_error = true;
        pod.text = "tool failed";
        tools::MCPClient probe_client;
        probe_client.url = pod.url();
        const bool client_reports = probe_client.call_ex("mazemaker_recall", core::json_obj()).is_error;
        if (client_reports) {
            m.recall("a question the tool rejects", 3);
            chk(!m.up(), "maze: a tool that reports isError marks it down");
            pod.is_error = false;
            pod.text = "";
            m.recall("again", 3);
            chk(m.up(), "maze: the next answered call brings it back up");
        } else {
            std::printf("note: MCPClient::call_ex does not report isError yet; that part of the maze contract is not exercised\n");
            pod.is_error = false;
        }
        pod.shutdown();
        m.recall("nobody is listening", 3);
        chk(!m.up(), "maze: an unreachable pod marks it down");
    }

    if (g_fail) std::printf("%d FAILED\n", g_fail);
    else std::printf("all maze tests passed\n");
    return g_fail ? 1 : 0;
}
