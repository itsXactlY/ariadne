#pragma once
// dd/context/maze.hpp — mazemaker as the tape's memory, and the session as a
// chain in its graph.
//
// The tape forgets on purpose: what ages out of the window is dropped, and
// the maze gives it back. Every finished turn is soaked (auto:turn:<session>)
// and every reasoning chain the first time it is sent (auto:reasoning:…).
// Tool calls and tool output never go in — a graph full of build logs finds
// nothing. Turns that produced no answer never go in either.
//
// The session is written as a graph, not a pile of rows: labels are unique
// per session, each write hands its id back (the ledger the engine keeps),
// and edges chain it — turn → next turn (session_next), reasoning → the turn
// it served (reasoned_in) — so a rebase names ids instead of searching, a
// resume walks the chain, and the dream cycles see a session's shape.
//
// Soaking is fire-and-forget: callers push onto a lock-free queue, one worker
// writes and pushes the ids back on another. Disk is the fallback only while
// the pod is dead: a spool (edges included), replayed one write at a time.
// ~Soaker stops that worker and joins it: teardown waits, it never polls.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dd/context/memory.hpp"
#include "dd/core/slot_ring.hpp"

namespace dd {
namespace context {

// The mazemaker pod over MCP (stateless calls, safe from any thread).
class Maze : public Memory {
public:
    explicit Maze(std::string mcp_url);   // "" = no memory backend
    bool configured() const override { return !url_.empty(); }
    bool up() const override { return up_.load(std::memory_order_relaxed); }
    bool probe() override;

    std::vector<Hit> recall(const std::string& query, int limit, const std::string& scope = "") override;
    long remember(const std::string& label, const std::string& content) override;
    std::vector<Hit> think(long id, int depth) override;
    std::vector<Hit> get(const std::vector<long>& ids) override;
    std::vector<Hit> facts(long source_id, int limit) override;
    std::string intent(const std::string& query) override;
    std::vector<Hit> labels(const std::string& prefix, int offset, int limit) override;
    int connect(const std::vector<Edge>& edges) override;

    long recall_timeout_ms = 30000;

private:
    std::string url_, health_;
    std::atomic<bool> up_{false};
    std::string call(const std::string& tool, const std::string& args_json, long timeout_ms, bool mark = true);
};

// Hits fenced as recalled background, never new user input.
std::string context_block(const std::string& body);

// What a soak wrote: the engine's ledger of the session in the graph.
struct LedgerEntry {
    std::string kind;   // "turn" | "reasoning"
    std::string label, session;
    long id = 0;
    int epoch = 0;      // the tape epoch it was written in: still in the window while it lasts
};

class Soaker {
public:
    Soaker(std::shared_ptr<Memory> memory, std::string spool_path);
    ~Soaker();
    Soaker(const Soaker&) = delete;
    Soaker& operator=(const Soaker&) = delete;

    // producer side: the agent loop's thread alone, which is why the hand-off below uses the
    // ring's single-writer path and never a CAS per job.
    void turn(const std::string& session, const std::string& user, const std::string& assistant, int epoch = 0);
    void reasoning(const std::string& session, const std::string& text, int epoch = 0);
    // A resumed session continues its chain from its last turn.
    void continue_chain(const std::string& last_turn_label) { prev_turn_ = last_turn_label; }
    // consumer side: what was written since (the engine's thread claims the published entries)
    bool take(LedgerEntry& e);

    struct Stats {
        uint64_t queued = 0, written = 0, spooled = 0, skipped = 0, edges = 0;
        uint64_t unledgered = 0;   // ledger entries the ring refused because the engine was not draining
    };
    Stats stats() const;

    static bool non_answer(const std::string& text);
    static std::string scrub_think(const std::string& text);
    static std::string turn_content(const std::string& session, const std::string& user,
                                    const std::string& assistant, long long unix_s);
    static std::string reasoning_content(const std::string& session, const std::string& text, long long unix_s);
    static std::string label(const char* kind, const std::string& session, long long unix_s);

    static constexpr size_t kMaxContent = 3200;   // the provider's MAX_CONTENT_CHARS

private:
    struct Job {
        std::string kind, session, label, content;
        int epoch = 0;
        std::vector<Edge> edges;   // written after the node, both ends already in the graph
    };
    struct State;   // shared with the worker; ~Soaker joins it
    std::shared_ptr<State> st_;
    std::thread th_;   // the soak worker
    // producer-only
    std::string prev_turn_;
    std::vector<std::string> reasoning_since_turn_;
    long long last_ts_turn_ = 0, last_ts_reasoning_ = 0;
    long long next_ts(long long& last);
    void push(Job j);
};

}  // namespace context
}  // namespace dd
