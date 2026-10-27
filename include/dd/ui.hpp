#pragma once
// dd/ui.hpp — dd_tui's screen: a fixed-geometry panel layout in the Daedalus
// skin, painted into a cell grid and blitted as minimal-diff truecolor ANSI.
//
//   Canvas — cell grid: UTF-8 in, wide-char aware, boxes with titled borders.
//   Frame  — Canvas + the last presented grid; present() emits changed cells.
//   Theme  — palette, spinner, branding from ~/.daedalus/skins/<skin>.yaml.
//   Scene  — transcript cards + telemetry laid out as transcript | sidebar
//            over a prompt box and a status row. The same cards print inline
//            when there is no tty.
//
// Threads: no locks, and no queue — not even a lock-free one. A queue allocates
// a node per push, so an event queue made every streamed token an allocation.
// Feed calls (user/say/tool_*/note/llm_*/turn_*) publish a payload into a fixed
// slot and may come from any thread, the tool workers included; a streamed
// card's text is not an event at all, it lands in a fixed slot of bytes that
// only the streaming thread fills. Either way a hand-off is one atomic word:
// published by a release store, claimed by a CAS on the exact word the claimer
// read. The render thread owns every piece of scene state and drains both
// rings at the top of present()/drain_inline(). A publish the ring refuses is
// counted, never dropped quietly: dropped_events()/dropped_live_bytes().

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "dd/core/slot_ring.hpp"

namespace dd {
namespace ui {

// Colours are 0xRRGGBB tagged with bit 24; kNone is the terminal's default.
inline constexpr uint32_t kNone = 0;
constexpr uint32_t rgb(uint32_t hex) { return (hex & 0xFFFFFFu) | 0x1000000u; }
uint32_t mix(uint32_t a, uint32_t b, double t);

inline constexpr uint32_t kBold = 1, kDim = 2, kItalic = 4, kUnder = 8;

// All-uint32, no padding: a Cell is 16 bytes the diff kernels compare as
// four dwords (ui_simd.hpp).
struct Style {
    uint32_t fg = kNone, bg = kNone, at = 0;
    bool operator==(const Style& o) const { return fg == o.fg && bg == o.bg && at == o.at; }
};

// ch == 0 marks the right half of a wide glyph.
struct Cell {
    char32_t ch = U' ';
    Style st;
    bool operator==(const Cell& o) const { return ch == o.ch && st == o.st; }
};
static_assert(sizeof(Cell) == 16 && std::has_unique_object_representations_v<Cell>);

int cp_width(char32_t cp);
int text_width(const std::string& s);
std::u32string from_utf8(const std::string& s);
std::string to_utf8(const std::u32string& s);
// Word-wrap to `width` columns; max_rows > 0 cuts with an ellipsis.
std::vector<std::string> wrap(const std::string& s, int width, int max_rows = 0);
// One line, at most `width` columns, ellipsis when cut.
std::string clip(const std::string& s, int width);
std::string sgr(const Style& st, bool truecolor);

class Canvas {
public:
    Canvas(int w = 0, int h = 0);
    int w() const { return w_; }
    int h() const { return h_; }

    void put(int x, int y, char32_t ch, Style st);
    // Writes until max_w columns (-1: canvas edge); returns columns used.
    int text(int x, int y, const std::string& s, Style st, int max_w = -1);
    // Per-column gradient a -> b -> c.
    int gradient(int x, int y, const std::string& s, uint32_t a, uint32_t b, uint32_t c, uint32_t at = 0);
    void fill(int x, int y, int w, int h, Style st, char32_t ch = U' ');
    void box(int x, int y, int w, int h, Style border, bool rounded = true);
    // "┤ label ├" set into a border row; returns columns used.
    int title(int x, int y, const std::string& label, Style label_st, Style border);
    int title_right(int right, int y, const std::string& label, Style label_st, Style border);
    void blit(const Canvas& src, int sy, int dx, int dy, int rows);
    const Cell& at(int x, int y) const { return cells_[idx(x, y)]; }
    std::string row_text(int y) const;
    std::string to_ansi(bool color, bool truecolor) const;  // rows as printed lines

protected:
    int w_ = 0, h_ = 0;
    std::vector<Cell> cells_;
    size_t idx(int x, int y) const { return static_cast<size_t>(y) * static_cast<size_t>(w_) + static_cast<size_t>(x); }
    Cell& ref(int x, int y) { return cells_[idx(x, y)]; }
};

class Frame : public Canvas {
public:
    Frame(int w, int h);
    void invalidate() { full_ = true; }
    std::string present(bool truecolor);

private:
    std::vector<Cell> prev_;
    bool full_ = true;
};

using core::SlotRing;

struct Theme {
    uint32_t border = rgb(0xB026FF), title = rgb(0xF000FF), accent = rgb(0xDA70D6),
             dim = rgb(0x7200A8), text = rgb(0xF2E9FF), hot = rgb(0xFF4DFF),
             label = rgb(0xC800FF), ok = rgb(0x39FFB6), error = rgb(0xFF4F81),
             warn = rgb(0xFFE66D), gold = rgb(0xFFBF00), prompt = rgb(0xF2E9FF),
             input_rule = rgb(0xB026FF), response = rgb(0xD000FF),
             session_label = rgb(0xFF66E8), session_border = rgb(0x7200A8),
             status_bg = rgb(0x10051C), select_bg = rgb(0x291044), muted = rgb(0x8A7AA8);
    std::vector<std::pair<std::string, std::string>> wings = {
        {"⟪◉", "◉⟫"}, {"⟪◈", "◈⟫"}, {"⟪⬡", "⬡⟫"}, {"⟪◌", "◌⟫"}, {"⟪⦿", "⦿⟫"}};
    std::vector<std::string> verbs = {"weaving synapses", "firing neurons", "mapping the maze",
                                      "consulting the archives", "following the corridor"};
    std::string agent_name = "Daedalus", response_label = "⟁ DAEDALUS ⟁",
                prompt_symbol = "◇ ❯ ", welcome = "⟁ The maze opens. The cortex remembers.",
                goodbye = "⟁ The doors close. The maze remains.";

    // Built-in neural palette overlaid by the skin file; a missing or
    // unreadable file leaves the defaults.
    static Theme load(const std::string& skin_yaml);
};

enum class Kind { User, Say, Tool, Note, Error, Rule, Reason, Memory };
// How reasoning cards show: a fixed-height live tail, every line, or one line.
enum class ReasonView { Tail, Full, Hide };
// How much a card shows: Minimal folds a card to its headline, Normal is the
// default glance, Verbose opens arguments, results, reasoning and accounting.
enum class Verbosity { Minimal, Normal, Verbose };
enum class Run { Pending, Running, Ok, Failed };

// A turn recall as its card shows it: one row per memory the block carries.
struct MemoryRow {
    char kind = 'r';   // 'r' recalled, 'l' linked in the graph, 'f' extracted fact, 'e' earlier turn
    long id = 0;
    std::string label, text;
    double score = 0;
};
struct RecallView {
    std::vector<MemoryRow> rows;
    std::string meta, skill;   // the one-line summary; the skill offered, if any
};

struct Entry {
    Kind kind = Kind::Note;
    std::string text;        // body; for a tool: its JSON arguments
    std::string name;        // tool name
    std::string call_id;     // tool_call id
    std::string result;      // tool result
    Run run = Run::Pending;
    bool resolved = false;   // tool: result attached; streamed say/reason: call ended
    double ms = 0;           // tool runtime / llm latency
    std::string clock;       // HH:MM:SS
    uint64_t token = 0;      // tool_begin token
    long n = 0;              // streamed deltas (≈ tokens)
    unsigned ver = 0;
    std::shared_ptr<const RecallView> recall;   // memory card
};

struct LlmStats {
    double latency_ms = 0, gen_tps = 0, pp_tps = 0;
    long prompt_tokens = 0, completion_tokens = 0, cached_tokens = 0;
    int attempts = 1;   // sends this call needed; shown only when the endpoint made it re-send
};

// The context tape as the sidebar shows it (dd/context TapeStats, flattened).
struct TapeView {
    bool on = false;
    int epoch = 0, visible = 0, deferred = 0, main_slot = 0;
    long history = 0, tape = 0, tokens = 0, rebase_at = 0, prompt = 0, reused = 0;
    std::string level = "ok", prep, server;
    bool maze = false;
    unsigned long long soaked = 0, soak_queue = 0, spooled = 0, unledgered = 0;
    int steered = 0;   // prompts folded into the running turn at round boundaries
    long ledger = 0, edges = 0;   // this session's nodes and edges in the graph
};

// The work ledger the model keeps with the todo tool, as the sidebar shows it.
struct PlanView {
    enum Marker : uint8_t { Pending, Active, Done, Dropped };
    std::vector<std::pair<Marker, std::string>> items;   // the model's own order
    int open = 0, total = 0;
};

struct Info {
    std::string model, endpoint, effort = "default";
    int tools = 0, max_turns = 0;
    long n_ctx = 0;
    std::vector<std::string> sources;  // "mazemaker 34", "local 4"
};

class Scene {
public:
    explicit Scene(Theme th);

    // ---- feed side: posts an event, any thread ----
    void set_info(const Info& info);
    void set_max_turns(int n);
    void user(const std::string& text);
    void say(const std::string& text);
    void tool_call(const std::string& id, const std::string& name, const std::string& args);
    uint64_t tool_begin(const std::string& name);   // binds to the first pending call of that name
    void tool_end(uint64_t token, double ms, bool ok);
    void tool_result(const std::string& id, const std::string& text);
    void note(const std::string& text);
    void recalled(const RecallView& v);   // a memory card
    void error(const std::string& text);
    void clear();
    void llm_begin();
    // Streamed thinking / streamed answer. A delta is not an event: its bytes go into a fixed
    // slot the streaming thread owns alone, and the render thread claims it. Nothing is
    // allocated per token; a delta the ring refuses is counted, not dropped quietly.
    void llm_reasoning(const std::string& delta);
    void llm_content(const std::string& delta);
    void llm_end(const LlmStats& s);
    void set_effort(const std::string& label);
    void set_tape(const TapeView& t);
    void set_plan(const PlanView& p);   // the todo ledger; empty clears the panel
    void recall(bool active);                       // the turn's recall is running
    void turn_begin();
    void turn_end();

    // ---- render side: the owning thread only ----
    void set_input(const std::u32string& line, size_t cursor, const std::string& hint);
    void scroll(int rows);              // + = older
    void scroll_end() { scroll_ = 0; }
    void set_reason_view(ReasonView v) { view_ = v; }
    ReasonView reason_view() const { return view_; }
    void set_verbosity(Verbosity v) { verb_ = v; }
    Verbosity verbosity() const { return verb_; }
    void redraw() { invalidate_all(); }
    // Drop every cached card block: geometry changed around the cards, so
    // all of them must go through render() again.
    void invalidate_all() {
        if (!frame_) return;
        frame_->invalidate();
        for (auto& c : cache_) c.tw = -1;
    }
    // One synchronized frame: changed cells + cursor at the prompt; empty
    // when nothing changed.
    std::string present(int cols, int rows, bool truecolor);
    // Cards completed since the last call, as printed lines (no tty).
    std::string drain_inline(int width, bool color, bool truecolor);
    std::string row_text(int y) const;  // last presented frame, plain text
    size_t live_text_bytes() const;     // bytes held by the streaming reasoning/answer cards (render thread, after present())
    // Publishes the rings turned away: lifecycle events that found the event ring full, and
    // streamed bytes that found a stream ring full. Both are named in the reply, never lost
    // without a number attached.
    uint64_t dropped_events() const;
    uint64_t dropped_live_bytes() const;
    const Theme& theme() const { return th_; }

    static constexpr int kSidebarW = 34, kCardMaxW = 112, kMinCols = 60, kMinRows = 14, kReasonRows = 6;

private:
    using clock = std::chrono::steady_clock;
    // Everything but the streamed text: a delta is bytes in a stream slot, not an event.
    enum class Ev : uint8_t { Info, MaxTurns, User, Say, ToolCall, ToolBegin, ToolEnd, ToolResult,
                              Note, Error, Clear, LlmBegin, LlmEnd, Effort,
                              Tape, Recall, Memory, TurnBegin, TurnEnd, Plan };
    struct Event {
        Ev ev = Ev::Note;
        uint64_t stamp = 0;      // order_ stamp: where this event sits against the streamed runs
        std::string a, b, c;
        uint64_t token = 0;
        double ms = 0;
        bool ok = false;
        LlmStats llm;
        std::shared_ptr<const Info> info;
        std::shared_ptr<const TapeView> tape;
        std::shared_ptr<const RecallView> recall;
        std::shared_ptr<const PlanView> plan;
        clock::time_point at{};
    };
    enum class Phase { Idle, Recalling, Thinking, Reasoning, Writing, Tools };

    // ---- feed side: any thread ----
    const Theme th_;

    // A lifecycle event, in a fixed slot. kEventSlots is sized for a turn's worth of tool
    // cards and notes with room to spare; a full ring refuses and counts.
    static constexpr size_t kEventSlots = 256;
    SlotRing<kEventSlots, Event> q_;
    std::atomic<uint64_t> next_token_{1};   // the tool cards' tokens; a fetch_add, no counter lock

    // The feed order, stamped once per hand-off whether it is an event or a run of streamed bytes.
    // Two channels would otherwise lose the causal order between them — the answer's first byte
    // has to land after the last reasoning byte and before llm_end, and only a shared counter can
    // say which came first. A fetch_add; no lock, no queue.
    std::atomic<uint64_t> order_{1};

    // A streamed card's text: one fixed-capacity byte buffer per stream, filled by the thread
    // that streams it. The buffer has exactly one writer — the streaming thread — so appending a
    // delta is a plain copy, no CAS, no node, no allocation. Two words are shared and each has
    // one writer: `len`, how many bytes have been written (release, so the bytes arrive with the
    // length), and `read`, how many the render thread has taken. A byte is written only where the
    // reader has already passed or where it cannot yet reach, so the writer never touches a byte
    // the reader is reading; a delta that would land between those is refused and counted.
    // Positions are absolute and monotonic across calls, so a card's text never restarts.
    struct LiveBuf {
        static constexpr size_t kCap = 512u << 10;  // fixed, and equal to the cap the live cards already had
        std::array<char, kCap> bytes{};             // the streaming thread writes, the render thread reads published bytes
        std::atomic<uint64_t> len{0};               // bytes written; release after the bytes land
        std::atomic<uint64_t> read{0};              // bytes the render thread has taken
        // Everything below is stamped by the writer and published by the store on `len`, so the
        // render thread sees a run's bytes, its feed order, its clock and its token count together.
        std::atomic<uint64_t> run_start{0};         // order_ stamp of this run's first byte
        std::atomic<uint64_t> run_end{0};           // order_ stamp of this run's last byte
        std::atomic<uint64_t> deltas{0};            // deltas in this run — what the card counts as tokens
        std::atomic<uint64_t> first_ns{0};          // when this run's first byte landed, ns from the steady epoch
    };
    LiveBuf reason_, say_;
    std::atomic<uint64_t> dropped_live_{0};   // bytes a full stream buffer refused because the reader was behind

    // everything below: render thread only — the agent thread never touches any of it; what it
    // hands over arrives through q_ (a SlotRing) and is claimed here.
    std::deque<Entry> entries_;   // render thread: the scene's own log, drained by drain_inline
    struct Cache { int w = -1, tw = -1; unsigned ver = 0; int spin = -1, view = -1, verb = -1; Canvas block; };
    std::deque<Cache> cache_;    // render thread: one canvas per Entry, positionally paired with entries_
    size_t printed_ = 0;     // drain_inline cursor (index into entries_)
    Info info_;
    TapeView tape_;
    PlanView plan_;
    LlmStats last_{};
    std::deque<double> tps_hist_;   // render thread: the last 24 measured rates, oldest first
    long calls_total_ = 0;
    int turn_calls_ = 0, turns_ = 0, tools_turn_ = 0;
    Phase phase_ = Phase::Idle;
    bool busy_ = false;
    std::string running_tool_;
    Entry* live_reason_ = nullptr;   // this call's streaming cards (newest entries)
    Entry* live_say_ = nullptr;
    clock::time_point turn_t0_{}, phase_t0_{}, reason_t0_{};
    double last_turn_ms_ = 0;

    std::u32string input_;
    size_t cursor_ = 0;
    std::string hint_;
    int scroll_ = 0, tick_ = 0;
    ReasonView view_ = ReasonView::Tail;
    Verbosity verb_ = Verbosity::Normal;
    std::unique_ptr<Frame> frame_;
    std::string last_tail_;

    void post(Event e);
    void stream(LiveBuf& b, const std::string& delta);   // the streaming thread's side
    // The render thread's side: flush whole runs, oldest feed-order first, never past `stamp`.
    void flush_before(uint64_t stamp);
    void flush_run(LiveBuf& b, Kind kind, Entry*& live);
    void apply();
    void apply(Event& e);
    Entry& push(Kind k, const std::string& text);
    void close_live(clock::time_point at, double latency_ms);
    bool stale(size_t i, int w) const;
    const Canvas& block(size_t i, int w);
    Canvas render(const Entry& e, int w) const;
    double work_ms() const;
    void paint_transcript(Frame& f, int x, int y, int w, int h);
    void paint_splash(Frame& f, int x, int y, int w, int h) const;
    void paint_sidebar(Frame& f, int x, int y, int w, int h) const;
    void paint_prompt(Frame& f, int x, int y, int w, int& cur_x) const;
    void paint_status(Frame& f, int y, int w) const;
};

}  // namespace ui
}  // namespace dd
