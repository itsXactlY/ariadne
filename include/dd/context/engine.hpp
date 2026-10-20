#pragma once
// dd/context/engine.hpp — the context tape, wired into the agent loop.
//
//   turn start   append the question; past the hard watermark adopt the
//                prewarmed epoch or rebase now; one recall for the turn,
//                rendered onto the question once
//   every call   the frozen tape plus what was appended since; the system
//                prompt and the tools array never change, so the server
//                re-reads only the new messages
//   after tools  past the hard watermark: rebase inside the turn
//   turn end     soak the turn; past the high watermark prewarm the next
//                epoch while the operator reads (sidekick slot on a
//                multi-slot llama-server, parked and cancellable on Strata)
//
// A rebase builds turn 0 from retrieval, not from a summary: the question,
// an anchor that says where the rest is, what this turn already ran, one
// recall, and the unfinished tail. Watermarks count server tokens across
// both slots against the fast layer (compression.fast_layer_tokens).
// Port of run_agent.py's payload tape, retrieval rebase and hot-swap.

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dd/agent/agent.hpp"
#include "dd/agent/http_llm.hpp"
#include "dd/context/deck.hpp"
#include "dd/context/endpoint.hpp"
#include "dd/context/maze.hpp"
#include "dd/context/render.hpp"
#include "dd/context/tape.hpp"

namespace dd {
namespace context {

struct EngineConfig {
    RenderPolicy render;
    bool tape = true;               // context.payload_tape: false renders every call from scratch
    long fast_layer_tokens = 0;     // compression.fast_layer_tokens; 0 = no fast-layer watermarks
    double low = 0.60, high = 0.80, hard = 0.90;   // compression.watermarks
    double ceiling = 0.85;          // without a fast layer: rebase at this share of n_ctx
    int protect_last_n = 6;         // compression.protect_last_n: the tail a rebase keeps
    bool turn_recall = true;        // memory.turn_recall: one recall per turn on the question
    int recall_limit = 6;
    long recall_timeout_ms = 30000;
    bool prewarm = true;            // compression.prewarm: next epoch between turns
};

struct TapeStats {
    int epoch = 0;
    size_t history = 0, tape = 0;   // session messages, rendered messages on the tape
    long tokens = 0, pool = 0, rebase_at = 0, high_at = 0, fast_layer = 0;
    long prompt = 0, reused = 0;    // the last call: prompt tokens, of which served from cache
    std::string level = "ok", prep, server;
    int visible = 0, deferred = 0, main_slot = 0;
    bool maze = false;
    uint64_t soaked = 0, soak_queue = 0, spooled = 0;
    std::string session;
};

class TapeEngine : public agent::ContextEngine {
public:
    TapeEngine(EngineConfig cfg, std::string system_prompt, Endpoint& ep, std::shared_ptr<Maze> maze,
               Soaker* soaker, ToolDeck& deck, agent::HttpLLM& llm, std::string spill_root);
    ~TapeEngine() override;

    // ---- agent loop (the worker thread) ----
    void begin_turn(const std::string& user_message) override;
    std::vector<core::Message> payload() override;
    core::Json tools() override { return deck_.definitions(); }
    void on_assistant(const core::Message& m) override;
    void on_tool_results(const std::vector<core::Message>& results) override;
    void after_tools() override;
    void end_turn(const std::string& final_response, bool interrupted) override;

    void on_call(const agent::CallStats& st);   // tokens as the server counted them
    // ---- between turns (no turn running) ----
    void reset();                                // a fresh session
    void rebase_now();                           // /rebase
    TapeStats stats() const;
    const Session& session() const { return *session_; }

    std::function<void(const std::string&)> on_note;      // any thread
    std::function<void(const TapeStats&)> on_stats;
    std::function<void(bool)> on_recall;                  // a turn recall started / ended

private:
    struct Prep {
        std::vector<ItemPtr> base;
        size_t snapshot_len = 0;
        int generation = 0;
        int slot = -1;        // multi-slot: the slot that holds it
        bool warmed = false;
    };

    EngineConfig cfg_;
    std::string system_;
    Endpoint& ep_;
    std::shared_ptr<Maze> maze_;
    Soaker* soaker_;
    ToolDeck& deck_;
    agent::HttpLLM& llm_;
    std::string spill_root_;
    std::unique_ptr<Session> session_;
    std::unique_ptr<Spill> spill_;
    Renderer render_;
    Tape tape_;
    std::string injection_, question_;
    long tokens_ = 0, prompt_ = 0, reused_ = 0;
    size_t tool_chars_ = 0;   // results appended since the last call

    std::atomic<Prep*> ready_{nullptr};
    std::atomic<bool> prep_running_{false}, prep_cancel_{false};
    std::thread prep_thread_;

    long rebase_at() const;
    long high_at() const;
    std::string level(long pool) const;
    void soak(Item& item);
    void note(const std::string& s) const;
    void emit() const;
    void watermark(bool turn_start);
    void rebase(const char* why);
    bool adopt(long pool, bool turn_start);
    void start_prep();
    void join_prep(bool cancel);
    // ratio: server tokens per chars/4, so the new epoch is sized as the server will count it
    std::vector<ItemPtr> retrieval_base(const std::vector<ItemPtr>& items, const std::string& session_id,
                                        bool* recalled, double ratio) const;
    long prefix_tokens(double ratio) const;   // system prompt + tools array
    long epoch_tokens(const std::vector<ItemPtr>& base, double ratio) const;   // a new epoch, as the server counts it
    int skipped_gen_ = -1;   // the epoch whose rebase could not make room (said once)
};

// What a dropped stretch of this turn already did, one line per call.
std::string turn_so_far(const std::vector<ItemPtr>& dropped);
// A short question gets the previous real user turn as an anchor.
std::string anchored_query(const std::vector<ItemPtr>& items, long current);
bool trivial_prompt(const std::string& text);

}  // namespace context
}  // namespace dd
