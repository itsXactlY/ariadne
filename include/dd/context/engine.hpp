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
#include <set>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dd/agent/agent.hpp"
#include "dd/agent/http_llm.hpp"
#include "dd/context/deck.hpp"
#include "dd/context/endpoint.hpp"
#include "dd/context/maze.hpp"
#include "dd/context/memory.hpp"
#include "dd/context/recall.hpp"
#include "dd/context/render.hpp"
#include "dd/context/tape.hpp"
#include "dd/core/snap_cell.hpp"
#include "dd/know/router.hpp"

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
    uint64_t soaked = 0, soak_queue = 0, spooled = 0, edges = 0;
    size_t ledger = 0, skills = 0;   // this session's nodes in the graph, skills indexed
    int steered = 0;                 // prompts folded in mid-turn since the turn began
    std::string session, recall;     // the last turn recall, summarised
};

class TapeEngine : public agent::ContextEngine {
public:
    TapeEngine(EngineConfig cfg, std::string system_prompt, Endpoint& ep, std::shared_ptr<Memory> memory,
               Soaker* soaker, ToolDeck& deck, agent::HttpLLM& llm, std::string spill_root,
               know::Router* router = nullptr);
    ~TapeEngine() override;

    // ---- agent loop (the worker thread) ----
    void begin_turn(const std::string& user_message) override;
    std::vector<core::Message> payload() override;
    core::Json tools() override { return deck_.definitions(); }
    void on_assistant(const core::Message& m) override;
    void on_tool_results(const std::vector<core::Message>& results) override;
    void after_tools() override;
    void on_steering(const std::vector<core::Message>& notes) override;   // mid-turn input, folded here
    void end_turn(const std::string& final_response, bool interrupted) override;
    bool on_overflow(const std::string& error) override;   // the server refused the window: rebase, send again

    void on_call(const agent::CallStats& st);   // tokens as the server counted them
    // ---- between turns (no turn running) ----
    void reset();                                // a fresh session
    void rebase_now();                           // /rebase
    // Rebuild a session from its auto:turn chain in the maze (a dd session or
    // a Python Daedalus one): the maze is the session store.
    bool resume(const std::string& session_id, std::string* why = nullptr);
    struct SessionRef {
        std::string id, last;   // last: UTC of its newest turn
        int turns = 0;
    };
    std::vector<SessionRef> recent_sessions(const std::string& prefix = "dd-", int limit = 8) const;
    // Any thread: the last snapshot the owning thread published (every emit()); a default one before the first.
    TapeStats stats() const;
    void sync();                                 // between turns: take in what the soak worker wrote
    const Session& session() const { return *session_; }
    double ratio() const { return render_.ratio(); }   // server tokens per chars/4, as measured

    std::function<void(const std::string&)> on_note;      // any thread
    std::function<void(const TapeStats&)> on_stats;
    std::function<void(bool)> on_recall;                  // a turn recall started / ended
    // what a turn recall brought, and its one-line summary; unset: the summary goes to on_note
    std::function<void(const Prefetch&, const std::string&)> on_prefetch;

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
    std::shared_ptr<Memory> mem_;
    Soaker* soaker_;
    know::Router* router_;   // what to look up, and when; null: the pod's recall at a rebase only
    Recaller recaller_;      // the rebase's recall when there is no router
    std::vector<core::ToolCall> pending_calls_;   // the assistant's calls, each to meet its result
    std::vector<know::CallRecord> records_;       // this round's calls and what came back
    std::vector<LedgerEntry> ledger_;   // this session's nodes in the graph, oldest first
    std::string last_recall_;
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
    int steered_ = 0;         // prompts folded in at round boundaries since begin_turn
    bool turn_open_ = false;  // begin_turn ran and end_turn has not: end_turn is idempotent
    mutable core::SnapCell<TapeStats> snap_;
    bool drop_pending_ = false;   // a rebase left the old epoch parked: drop it after the next answer

    std::atomic<Prep*> ready_{nullptr};
    std::atomic<bool> prep_running_{false}, prep_cancel_{false};
    std::thread prep_thread_;

    long window() const;       // what the server holds: /props, else the config's, else what a refusal told
    long fast_layer() const;   // compression.fast_layer_tokens, never past the window
    long rebase_at() const;
    long high_at() const;
    std::string level(long pool) const;
    void soak(Item& item);
    void append_note(const std::string& text);   // a hidden user item after the tool results, rendered once
    void speak_after_tools();                    // the router's notes for a finished batch
    void note(const std::string& s) const;
    TapeStats compute_stats() const;             // the owning thread's
    void publish() const;                        // one publisher at a time: the worker in a turn, the caller between turns
    void emit() const;
    void watermark(bool turn_start);
    bool rebase(const char* why);   // false: no room could be made, the history stands
    bool adopt(long pool, bool turn_start);
    void start_prep();
    void join_prep(bool cancel);
    // ratio: server tokens per chars/4, so the new epoch is sized as the server will count it
    std::vector<ItemPtr> retrieval_base(const std::vector<ItemPtr>& items, const std::string& session_id,
                                        bool* recalled, double ratio, const std::vector<long>& turn_ids) const;
    void drain_ledger();
    std::set<long> in_window() const;              // ids written in this epoch: on the tape now
    std::vector<long> turn_ids(bool earlier_only) const;
    long prefix_tokens(double ratio) const;   // system prompt + tools array
    long epoch_tokens(const std::vector<ItemPtr>& base, double ratio) const;   // a new epoch, as the server counts it
    std::atomic<long> window_seen_{0};   // the window a refusal named, for a server that does not report it
    int skipped_gen_ = -1;   // the epoch whose rebase could not make room (said once)
};

// What a dropped stretch of this turn already did, one line per call.
std::string turn_so_far(const std::vector<ItemPtr>& dropped);
// A short question gets the previous real user turn as an anchor.
std::string anchored_query(const std::vector<ItemPtr>& items, long current);
bool trivial_prompt(const std::string& text);
// A refused request's message (OpenAI's, llama-server's, Strata's wording): the tokens the server counted and the
// window it has, 0 where the message does not say. false: the error is not about the context.
bool parse_overflow(const std::string& what, long* prompt, long* window);

}  // namespace context
}  // namespace dd
