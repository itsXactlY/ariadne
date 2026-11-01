#pragma once
// dd/limbo/shade.hpp — the contract every dweller of the limbo level answers to.
//
// The limbo level is where a model lives before a turn baptises it: one front, several
// dwellers, and no consumer ever sees "two models". A shade is one dweller — a process
// this level owns, spawned, watched, and re-spawned. Two kinds exist:
//
//   Strata — the packed-MoE engine (expert arena, TQ1_0 trellis MMQ, expert-profile
//          routing, resident experts, int8 KV residency, an MTP draft). llama.cpp cannot
//          serve these models at all, so this shade is not a fallback: it is the only way.
//   Llama  — llama.cpp-adaptive-kv-streaming, for dense models and for KV-cache streaming
//          over the PCIe seam. It is the fallback, and it stays a real server: its own
//          template, its own tokenizer, its own slots.
//
// What the two have in common is only this contract. Everything else — how a turn is sent
// (a control line on a pipe, or an SSE proxy), how a prompt is tokenised (ids in, or
// messages out), how memory is freed — stays inside the shade.
//
// The contract is a turn, not a socket. The level owns admission; a shade is handed a turn
// it already owns and may assume nobody else is speaking to its process.

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "dd/core/json.hpp"
#include "dd/core/message.hpp"

namespace dd {
namespace limbo {

// None means "no preference": the level picks the dweller that can answer what it was given.
enum class ShadeKind { None, Strata, Llama };

const char* shade_kind_name(ShadeKind k);

// One dweller's shape: what to spawn, what it answers as, where its stderr goes.
struct ShadeSpec {
    ShadeKind kind = ShadeKind::None;
    std::string name;          // the dweller's own name in /slots and /metrics: "strata", "llama"
    std::string model_id;      // what /v1/models lists and chunks name
    std::vector<std::string> aliases;   // other names that route to this dweller
    std::string exe;           // engine/strata, or llama-server
    std::vector<std::string> args;
    std::string cwd;
    std::string log;           // stderr's file; "" sends it to /dev/null
    std::vector<std::pair<std::string, std::string>> env;
    std::string base_url;      // Llama only: the server to proxy (http://127.0.0.1:PORT)
    int port = 0;              // Llama only: the port to bind when this level spawns it
    int ready_timeout_s = 0;   // 0: no ceiling on the load; a load past it ends the process
    int silence_s = 120;       // Strata: how long a quiet engine may be before the turn dies
    int n_ctx = 0;             // 0: whatever the dweller reports (READY / /props)
};

// Sampling one turn asks for. Absent fields stay the engine's default, which for Strata is
// greedy; temperature 0 means the same thing and is not forwarded.
struct Sampling {
    double temperature = 0.0;
    double top_p = 1.0;
    int top_k = -1;  // -1: the engine's own default. 0 means "off" was asked for, which is not
                     // the same thing as "nothing was said" — a default of 0 would forward a
                     // sampling change on every turn nobody asked to sample.
    double min_p = 0.0;
    double repetition_penalty = 1.0;
    double frequency_penalty = 0.0;
    double presence_penalty = 0.0;
    int penalty_last_n = 0;   // 0: the engine's own window (its last 64 tokens)
    double pcie_frac = -1.0;  // -1: the engine's setting. Per-turn tuning, not a restart.
    double spec_min_p = -1.0; // -1: the engine's setting
    long seed = -1;      // -1: the engine picks one
    int max_tokens = -1; // -1: until the window or a stop token
    std::string effort;  // "" = the dweller's default. "none" is thinking disabled, which
                         // both dwellers read the same way.
};

// What the dweller says about itself right now. `n_ctx` is the ceiling Minos judges a
// prompt against; `facts` is whatever the engine volunteered (INFO key=value).
struct Props {
    int n_ctx = 0;
    std::string model;
    ShadeKind kind = ShadeKind::Strata;
    core::Json facts = core::json_obj();
    int slots = 1;   // Llama's --np; Strata's --batch slots. One, unless the dweller says more.
    bool alive = false;
};

// One streamed piece of an answer. Text, not ids: a shade's own decoder is what makes a
// piece speakable, and the level never handles ids.
struct Delta {
    std::string content;
    std::string reasoning;
};

// The producer hands out each piece as it exists. false means "stop producing": the consumer
// went away mid-turn. A stream with no way to be told to stop is a stream that keeps generating
// for a peer that is no longer reading — tokens spent on nobody.
using DeltaSink = std::function<bool(const Delta&)>;

// The figures a turn finished with. Names mirror the engine's DONE line so a figure can be
// traced back to the line that produced it.
struct TurnResult {
    std::string finish;              // "stop", "length", "canceled", ...
    // The answer as assembled text. A streaming caller gets the pieces and never needs these;
    // a caller that asked for no stream collects them, and the level fills them from the sink.
    std::string content;
    std::string reasoning;
    long generated = 0;
    long prompt_tokens = 0;
    double prompt_ms = 0.0;
    double decode_ms = 0.0;
    long reused = 0;                 // prompt tokens the engine did not read again
    long drafts_offered = 0, drafts_accepted = 0;   // the MTP draft's hit rate
    long prompt_read = 0;            // #471: tokens actually read, after reuse
    long offloaded = 0;              // #588: routed experts read over PCIe
    core::Json tool_calls = core::json_null();   // Null: this dweller parsed nothing
};

// A turn a shade owns. Strata reads `ids` — the level tokenised the prompt; Llama reads
// `messages` and lets its own template do the work. Both are sent, so a shade never has to
// guess which one it is given.
struct TurnRequest {
    ShadeKind kind = ShadeKind::None;   // None: whatever is admitted answers
    std::string model_override;         // names a model; the dweller reporting that alias owns the turn
    std::vector<std::string> stop_seqs; // sequences the answer is cut at; each shade honours its own way
    std::vector<int> ids;
    std::vector<core::Message> messages;
    core::Json tools = core::json_null();
    Sampling sampling;
    const std::atomic<bool>* cancel = nullptr;   // set by whoever gave up on the turn
    bool stream = true;
};

// Engine ids -> text. The BPE lexicon (dd/limbo/lexicon.hpp) implements this; a shade with
// no decoder says so by name instead of half-answering.
struct Decoder {
    virtual ~Decoder() = default;
    virtual std::string piece(int id) const = 0;
    virtual bool stop_token(int id) const = 0;
};

class Shade {
public:
    virtual ~Shade() = default;

    virtual const ShadeSpec& spec() const = 0;

    // Spawn and wait for the dweller's own "I can take a turn" line. False leaves `error`
    // with the reason the level will show; the process is left dead, never half-alive.
    virtual bool start(std::string& error) = 0;

    virtual bool alive() const = 0;
    virtual Props props() const = 0;

    // Can this dweller speak a text turn at all? Strata needs the lexicon; without one it can
    // answer id turns only, and Minos must not hand it a message. A shade that cannot speak is
    // not a fallback — it is a dweller waiting for its tokenizer.
    virtual bool speaks_text() const { return true; }

    // One completion. Pieces go out through `sink` as they arrive; the finish and the
    // figures come back in `out`. false + `error` names what failed — including a dweller
    // that died mid-turn, which is a death at the process boundary, not a bad answer.
    virtual bool run(const TurnRequest& req, const DeltaSink& sink, TurnResult& out,
                     std::string& error) = 0;

    // Give up on the turn in flight. Idempotent, and safe from a thread that does not own
    // the pipe: the dweller is asked to stop producing, and the turn still runs to its end
    // line so the next request does not read this one's leftovers.
    virtual void interrupt() = 0;

    // End the dweller: it frees its own memory first (unpinning tens of GB takes seconds),
    // and the level waits for the exit before spawning again — a lost process that still
    // holds the arena is what makes the next spawn refuse to load.
    virtual void stop() = 0;
};

}  // namespace limbo
}  // namespace dd
