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
// Threads: no locks. Feed calls (user/say/tool_*/note/llm_*/turn_*) only post
// an event to a wait-free MPSC queue and may come from any thread, the tool
// workers included. The render thread owns every piece of scene state and
// applies the queue at the top of present()/drain_inline().

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "dd/core/mpsc.hpp"

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

using core::MpscQueue;

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
};

// The context tape as the sidebar shows it (dd/context TapeStats, flattened).
struct TapeView {
    bool on = false;
    int epoch = 0, visible = 0, deferred = 0, main_slot = 0;
    long history = 0, tape = 0, tokens = 0, rebase_at = 0, prompt = 0, reused = 0;
    std::string level = "ok", prep, server;
    bool maze = false;
    unsigned long long soaked = 0, soak_queue = 0, spooled = 0;
    long ledger = 0, edges = 0;   // this session's nodes and edges in the graph
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
    void llm_reasoning(const std::string& delta);   // streamed thinking
    void llm_content(const std::string& delta);     // streamed answer
    void llm_end(const LlmStats& s);
    void set_effort(const std::string& label);
    void set_tape(const TapeView& t);
    void recall(bool active);                       // the turn's recall is running
    void turn_begin();
    void turn_end();

    // ---- render side: the owning thread only ----
    void set_input(const std::u32string& line, size_t cursor, const std::string& hint);
    void scroll(int rows);              // + = older
    void scroll_end() { scroll_ = 0; }
    void set_reason_view(ReasonView v) { view_ = v; }
    ReasonView reason_view() const { return view_; }
    void redraw() { if (frame_) frame_->invalidate(); }
    // One synchronized frame: changed cells + cursor at the prompt; empty
    // when nothing changed.
    std::string present(int cols, int rows, bool truecolor);
    // Cards completed since the last call, as printed lines (no tty).
    std::string drain_inline(int width, bool color, bool truecolor);
    std::string row_text(int y) const;  // last presented frame, plain text
    const Theme& theme() const { return th_; }

    static constexpr int kSidebarW = 34, kCardMaxW = 112, kMinCols = 60, kMinRows = 14, kReasonRows = 6;

private:
    using clock = std::chrono::steady_clock;
    enum class Ev : uint8_t { Info, MaxTurns, User, Say, ToolCall, ToolBegin, ToolEnd, ToolResult,
                              Note, Error, Clear, LlmBegin, Reason, Content, LlmEnd, Effort,
                              Tape, Recall, Memory, TurnBegin, TurnEnd };
    struct Event {
        Ev ev = Ev::Note;
        std::string a, b, c;
        uint64_t token = 0;
        double ms = 0;
        bool ok = false;
        LlmStats llm;
        std::shared_ptr<const Info> info;
        std::shared_ptr<const TapeView> tape;
        std::shared_ptr<const RecallView> recall;
        clock::time_point at{};
    };
    enum class Phase { Idle, Recalling, Thinking, Reasoning, Writing, Tools };

    const Theme th_;
    MpscQueue<Event> q_;
    std::atomic<uint64_t> next_token_{1};

    // everything below: render thread only
    std::deque<Entry> entries_;
    struct Cache { int w = -1; unsigned ver = 0; int spin = -1, view = -1; Canvas block; };
    std::deque<Cache> cache_;
    size_t printed_ = 0;     // drain_inline cursor (index into entries_)
    Info info_;
    TapeView tape_;
    LlmStats last_{};
    std::deque<double> tps_hist_;
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
    std::unique_ptr<Frame> frame_;
    std::string last_tail_;

    void post(Event e);
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
