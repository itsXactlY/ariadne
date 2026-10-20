#pragma once
// dd/context/maze.hpp — mazemaker as the tape's memory.
//
// The tape forgets on purpose: what ages out of the window is dropped, and
// the maze gives it back. Every finished turn is soaked (auto:turn:<session>)
// and every reasoning chain the first time it is sent (auto:reasoning:…).
// Tool calls and tool output never go in — a graph full of build logs finds
// nothing. Turns that produced no answer never go in either. Soaking is
// fire-and-forget: callers push onto a lock-free queue, one worker writes.
// Disk is the fallback only while the pod is dead: a spool, replayed one
// write at a time once it answers again. Port of the mazemaker memory
// provider's sync_turn / soak_reasoning / prefetch.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dd/core/mpsc.hpp"
#include "dd/tools/mcp.hpp"

namespace dd {
namespace context {

class Maze {
public:
    explicit Maze(std::string mcp_url);   // "" = no memory backend
    bool configured() const { return !url_.empty(); }
    bool up() const { return up_.load(std::memory_order_relaxed); }
    bool probe();                          // the pod's /health

    struct Hit {
        std::string label, content;
        double similarity = 0;
    };
    // An empty result with up() true means the maze answered with nothing.
    std::vector<Hit> recall(const std::string& query, int limit, bool rerank, long timeout_ms);
    bool remember(const std::string& label, const std::string& content, long timeout_ms);
    // Hits fenced as recalled background, never new user input.
    static std::string context_block(const std::vector<Hit>& hits);

private:
    std::string url_, health_;
    std::atomic<bool> up_{false};
};

class Soaker {
public:
    Soaker(std::shared_ptr<Maze> maze, std::string spool_path);
    ~Soaker();
    Soaker(const Soaker&) = delete;
    Soaker& operator=(const Soaker&) = delete;

    void turn(const std::string& session, const std::string& user, const std::string& assistant);
    void reasoning(const std::string& session, const std::string& text);

    struct Stats {
        uint64_t queued = 0, written = 0, spooled = 0, skipped = 0;
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
        std::string label, content;
    };
    struct State;   // shared with the worker, which may outlive a quit by a write
    std::shared_ptr<State> st_;
    void push(Job j);
};

}  // namespace context
}  // namespace dd
