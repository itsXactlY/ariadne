#pragma once
// dd/limbo/charon.hpp — the crossing between this process and a dweller's pipe.
//
// Charon takes payment per crossing: here, tokens. The strata engine speaks one line per
// event on stdin and one line per event on stdout, and never says anything else:
//
//   READY <max_context> [stop]   it can take a turn; `stop` says a STOP line is honoured
//   INFO  key=value ...          facts about the load (kv, expert slots, version, ...)
//   GEN   <max_new> [k=v ...] id,id,...   the prompt, as ids; the engine's own sampling spelling
//   GENI  <max_new> [k=v ...] <embeddings> id,...   the same turn with an image's embeddings
//   T     <id>                   a token; its `LP <logprob> [...]` line follows in the same flush
//   PP    <read> <total> <ms> [tok_s]     the prompt is being read
//   RESUME <n>                   prompt tokens not read again (the engine's prefix cache)
//   DONE  <generated> <prompt> <prompt_ms> <decode_ms> <finish> [drafts...] [hits...] [tiers...] [read] [offloaded] [k=v...]
//   ERR   <reason>               the engine refused the request and is idle: no DONE follows
//   VRAM  <reserve_mib>          shrink/grow the expert cache between turns, answer with figures
//   STOP                         stop this turn; the turn still runs to its DONE
//   QUIT                         end the process; it frees its own memory first
//
// One request owns the pipe at a time (the level's admission decides who), so a writer is
// whoever holds the turn and a reader is the pump thread. There is no queue and no lock:
// lines move by SlotRing hand-off, and a thread with nothing to read parks in the kernel.
//
// A turn is never abandoned mid-stream. Whoever gives up still drains to the DONE line —
// otherwise the next request reads this one's leftovers, which is a wrong answer, not an
// empty one.

#include <atomic>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <sys/types.h>
#include <thread>
#include <vector>

#include "dd/core/json.hpp"
#include "dd/core/slot_ring.hpp"
#include "dd/limbo/shade.hpp"

namespace dd {
namespace limbo {

enum class LineKind { Ready, Info, Token, Logprob, Progress, Resume, Done, Err, Vram, Yielded, Batch, Other, Closed };

// One control line out of the engine, decoded. `raw` survives so a figure can be traced back
// to the line that produced it, and so a line this decoder does not know is never lost.
struct EngineLine {
    LineKind kind = LineKind::Other;
    long token = 0;                 // Token, Logprob's own id
    long read = 0, total = 0;       // PP
    double ms = 0.0, tok_s = 0.0;   // PP
    long n = 0;                     // RESUME / YIELDED
    core::Json fields = core::json_null();   // DONE figures, VRAM figures, INFO snapshot
    std::string raw;
};

// The figures of a DONE line, in the engine's field names. Mirrors server.py::_parse_done:
// positional fields first, then every key=value the engine added after them.
core::Json done_fields(std::string_view line);
// INFO key=value ... into an object. Values that read as integers stay integers.
core::Json info_fields(std::string_view line);

class Charon {
public:
    static constexpr size_t kLineRing = 4096;    // slots the pump may hold while a consumer reads
    static constexpr size_t kFactRing = 2;       // INFO snapshots kept for whoever asks

    explicit Charon(ShadeSpec spec);
    ~Charon();                 // stop(): the dweller ends, and its memory goes with it

    Charon(const Charon&) = delete;
    Charon& operator=(const Charon&) = delete;

    // Spawn the dweller and read up to its READY line. `ready_timeout_s` bounds the load: a
    // load past it is ended rather than held, so a hung engine cannot hold a turn forever.
    bool start(std::string& error);

    // STOP + QUIT, then terminate/kill; joins the pump. `grace_ms` is how long the dweller may
    // take to free its own memory: a loaded engine needs seconds, a dweller that never finished
    // loading needs none — it holds no arena yet, and waiting on it costs whoever is waiting.
    void stop(long grace_ms = 30000);
    bool alive() const;
    pid_t pid() const { return pid_.load(std::memory_order_acquire); }
    int exit_code() const { return exit_.load(std::memory_order_acquire); }
    int max_context() const { return ctx_.load(std::memory_order_acquire); }
    bool can_stop() const { return stop_ok_.load(std::memory_order_acquire); }
    const ShadeSpec& spec() const { return spec_; }

    // The newest INFO snapshot, published by the pump. Claims it: the caller owns the copy.
    // The snapshot goes back into the ring afterwards, so a dweller's facts survive a read:
    // whoever asks next gets the same set, not what is left over.
    // const: reading a snapshot moves it through the ring, which is what a cache is for. The
    // ring is mutable for exactly that reason — a const read that cannot republish would hand
    // the facts to one reader and lose them for the next.
    core::Json facts() const;
    // INFO snapshots the pump turned away because nobody read them. A number, never a silence.
    uint64_t facts_dropped() const { return facts_dropped_.load(std::memory_order_relaxed); }
    // Lines the line ring turned away. Zero while a turn streams; nonzero means a consumer stalled.
    uint64_t refused() const { return lines_.refused(); }

    // One command line to the dweller. A new GEN resets the STOP-once latch.
    bool send(std::string_view line, std::string& error);
    // Ask the dweller to stop this turn. Once per turn; a single write, so a thread that
    // does not own the pipe cannot interleave a partial line with a command.
    void stop_generation();

    // The next line, parked up to `timeout_ms`. false = nothing arrived (the caller decides
    // what quiet means), or the dweller ended — `dead()` says which.
    bool next(EngineLine& out, long timeout_ms);
    // Read to the turn's end line (DONE), discarding what it produces. Always run after an
    // abandoned turn. false + `error` when the dweller died or refused.
    bool drain_to_done(long timeout_ms, std::string& error);

    bool dead() const { return dead_.load(std::memory_order_acquire); }

private:
    void pump();               // the reader thread: bytes -> lines -> ring hand-off

    ShadeSpec spec_;
    std::atomic<pid_t> pid_{-1};
    std::atomic<int> exit_{-1};
    std::atomic<int> ctx_{0};
    std::atomic<bool> stop_ok_{false};
    std::atomic<bool> dead_{false};
    std::atomic<bool> stop_sent_{false};
    std::atomic<uint32_t> space_{0};      // claims since the pump last stalled
    std::atomic<uint32_t> ended_{0};      // bumped when the pump saw EOF: whoever waits for the
                                          // dweller's exit parks on it, never polls
    std::atomic<uint64_t> facts_dropped_{0};

    int in_fd_ = -1;           // write end: commands go down this pipe
    int out_fd_ = -1;          // read end: the dweller's stdout
    int log_fd_ = -1;          // the dweller's stderr, when a log was asked for
    // The compile-time ceiling is the capacity: a ring whose depth is a runtime knob is a ring
    // whose backpressure depends on who constructed it last.
    core::SlotRing<kLineRing, EngineLine> lines_;
    mutable core::SlotRing<kFactRing, core::Json> facts_;
    std::thread pump_;
    // INFO facts, cumulative since the load. The pump thread alone touches it; its snapshots
    // travel by ring, so nobody reads it and nothing is shared without an ownership hand-off.
    std::vector<std::pair<std::string, core::Json>> info_;

    void close_fds();
};

}  // namespace limbo
}  // namespace dd
