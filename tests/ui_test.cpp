// dd::ui — kernels vs scalar on every SIMD level the CPU has, the slot ring under
// contention, text metrics, frame diffing, layout geometry.
#include "dd/ui.hpp"
#include "dd/ui_simd.hpp"

#ifdef _OPENMP
#include <omp.h>   // the parallel-vs-serial check needs the runtime; the TSan build has none
#endif

#include <atomic>
#include <algorithm>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace dd::ui;

// Runs before every default-priority initializer, as another TU's static initializer might: the kernel set is picked on first use.
struct Early {
    int width;
    Early() : width(dd::ui::text_width("early ascii text")) {}
};
static Early g_early __attribute__((init_priority(101)));

// ---- allocation counter ----
// Replaceable allocation functions, so a hand-off's cost is measured rather than asserted. Both
// shapes below go through the same counter, and only the window between a snapshot pair is read, so
// the Scene's own construction and the payload strings the caller owns are never in the number.
static std::atomic<uint64_t> g_alloc{0};
static void* counted_new(size_t n) {
    if (n == 0) n = 1;                       // operator new must never return null for size 0
    g_alloc.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new(size_t n) { return counted_new(n); }
void* operator new[](size_t n) { return counted_new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

static int g_fail = 0;
static void chk(bool ok, const std::string& label) {
    if (!ok) { ++g_fail; std::printf("FAIL %s\n", label.c_str()); }
}

static std::vector<std::string> lines_of(const std::string& s) {
    std::vector<std::string> v;
    std::istringstream is(s);
    for (std::string l; std::getline(is, l);) v.push_back(l);
    return v;
}

static void kernels(const char* level, std::mt19937& rng) {
    if (!simd::force(level)) { std::printf("skip %s (cpu lacks it)\n", level); return; }
    std::printf("kernels: %s\n", simd::level());
    for (int n : {0, 1, 3, 4, 5, 15, 16, 17, 31, 64, 237, 1000}) {
        std::vector<Cell> a(static_cast<size_t>(n)), b;
        for (auto& c : a) c = Cell{static_cast<char32_t>(rng() % 0x3000), Style{static_cast<uint32_t>(rng() & 0xFFFFFFu), static_cast<uint32_t>(rng() & 0xFFFFFFu), static_cast<uint32_t>(rng() & 15u)}};
        b = a;
        std::vector<char> want(static_cast<size_t>(n)), got(static_cast<size_t>(n));
        int want_n = 0;
        for (int i = 0; i < n; ++i) {
            if (rng() % 5 == 0) {
                switch (rng() % 4) {   // one differing dword per changed cell, any lane
                case 0: b[static_cast<size_t>(i)].ch ^= 1; break;
                case 1: b[static_cast<size_t>(i)].st.fg ^= 1; break;
                case 2: b[static_cast<size_t>(i)].st.bg ^= 1; break;
                default: b[static_cast<size_t>(i)].st.at ^= 1; break;
                }
                want[static_cast<size_t>(i)] = 1;
                ++want_n;
            }
        }
        const int got_n = simd::diff_cells(a.data(), b.data(), n, got.data());
        chk(got_n == want_n && got == want, std::string(level) + " diff n=" + std::to_string(n));
    }
    for (int round = 0; round < 200; ++round) {
        const size_t n = rng() % 300;
        std::string s(n, 'a');
        for (auto& ch : s) ch = static_cast<char>(rng() % 0x80);
        size_t hi = n;
        if (n && rng() % 2) { hi = rng() % n; s[hi] = static_cast<char>(0xC3); }
        chk(simd::ascii_run(s.data(), n) == hi, std::string(level) + " run");
        size_t pr = 0;
        for (size_t i = 0; i < hi; ++i) pr += s[i] >= 0x20 && s[i] < 0x7F;
        chk(simd::ascii_printable(s.data(), hi) == pr, std::string(level) + " printable");
        std::vector<char32_t> w(hi + 1, U'#');
        simd::widen_ascii(s.data(), hi, w.data());
        bool ok = w[hi] == U'#';   // no write past n
        for (size_t i = 0; i < hi; ++i) ok = ok && w[i] == static_cast<unsigned char>(s[i]);
        chk(ok, std::string(level) + " widen");
    }
}

int main() {
    std::mt19937 rng(42);
    const std::string best = simd::level();
    for (const char* l : {"scalar", "avx2", "avx512"}) kernels(l, rng);
    simd::force(best.c_str());

    // ---- the slot ring: 8 producers, a consumer racing them ----
    // The ring is fixed, so a producer that arrives when every slot is held is refused; that is
    // the contract, and it is the whole reason the hand-off costs no allocation. What must hold
    // regardless: nothing lands twice, nothing lands out of its producer's order, and every item
    // is accounted for as either claimed or refused.
    {
        // The workload stays near the ring's capacity: a refused publish walks every slot looking
        // for a free one, so a workload far larger than the ring measures the refusal path, not
        // the hand-off. 8 producers x 2000 items against 1024 slots does both.
        SlotRing<1024, std::pair<int, int>> q;
        constexpr int P = 8, N = 2000;
        std::vector<std::thread> ts;
        std::vector<std::atomic<uint64_t>> refused(P);
        for (int p = 0; p < P; ++p) {
            refused[p].store(0);
            ts.emplace_back([&, p] {
                for (int i = 0; i < N; ++i)
                    if (!q.publish_any(std::pair<int, int>{p, i})) refused[p].fetch_add(1, std::memory_order_relaxed);
            });
        }
        std::vector<int> last(P, -1);
        uint64_t stamp = 0;
        bool order = true, monotonic = true;
        long got = 0;
        std::pair<int, int> v;
        uint64_t seq = 0;
        // The ring is smaller than the workload, so publishes are refused; the loop ends when every
        // item is accounted for as claimed or refused, not when it has claimed them all.
        while (got + static_cast<long>(q.refused()) < static_cast<long>(P) * N) {
            if (q.claim_one(v, &seq)) {
                order = order && static_cast<int>(v.second) > last[static_cast<size_t>(v.first)];
                last[static_cast<size_t>(v.first)] = static_cast<int>(v.second);
                monotonic = monotonic && seq > stamp;   // oldest first, always
                stamp = seq;
                ++got;
            }
        }
        for (auto& t : ts) t.join();
        uint64_t lost = 0;
        for (int p = 0; p < P; ++p) lost += refused[p].load();
        chk(order && monotonic && got + static_cast<long>(lost) == static_cast<long>(P) * N,
            "ring: every item claimed once or refused, per-producer order, oldest-first");
        chk(q.published() == 0 && q.refused() == lost, "ring: drained, refusals counted on the ring");
    }

    // ---- Scene: an over-fed event ring is a number, not a silent loss ----
    // kEventSlots is 256. Feeding past it without rendering is the real overload shape: the agent
    // thread keeps posting while nobody claims. Every event past the 256th slot must be counted by
    // dropped_events(), and the slots already held must still drain intact.
    {
        Scene s{Theme{}};
        const int posts = 300, slots = 256;
        for (int i = 0; i < posts; ++i) s.note("note " + std::to_string(i));
        chk(s.dropped_events() == static_cast<uint64_t>(posts - slots),
            "scene: an over-fed event ring counts every refusal (" + std::to_string(s.dropped_events()) + " of " + std::to_string(posts) + ")");
        const long lines = static_cast<long>(lines_of(s.drain_inline(200, false, false)).size());
        chk(lines <= slots && s.dropped_events() == static_cast<uint64_t>(posts - slots),
            "scene: the accepted events drain intact and the count stays (" + std::to_string(lines) + " lines)");
        chk(s.dropped_live_bytes() == 0, "scene: a stream that never overflowed reports no lost bytes");
    }

    // ---- Scene: a stream buffer the reader fell behind on counts the bytes it refused ----
    // kCap is 512 KiB and the render thread never claims, so the writer runs past the cap. The
    // refused bytes are counted instead of rewritten under a byte the reader is holding.
    {
        Scene s{Theme{}};
        s.llm_begin();
        const std::string chunk(1024, 'x');
        const int fed = 600;
        for (int i = 0; i < fed; ++i) s.llm_content(chunk);
        chk(s.dropped_live_bytes() > 0,
            "scene: a stream buffer at the cap counts the refused bytes (" + std::to_string(s.dropped_live_bytes()) + ")");
        chk(s.dropped_live_bytes() <= static_cast<uint64_t>(fed * 1024), "scene: the refused-byte count cannot exceed what was fed");
    }


    // ---- what a hand-off costs: allocations counted, both shapes ----
    // The shipping shape is the stream buffer and the ring's fixed slots; the shape W10b replaced is
    // a queue node per push plus a copy of the payload. Measured single-threaded, so the number is
    // the hand-off alone and not thread-startup noise.
    {
        Scene s{Theme{}};
        s.llm_begin();
        std::vector<std::string> deltas;
        deltas.reserve(200);
        for (int i = 0; i < 200; ++i) deltas.push_back("delta " + std::to_string(i) + " " + std::string(28, 'x'));
        // Warm the buffers first: the first write touches memory the Scene owns already.
        s.llm_content(deltas.front());
        s.drain_inline(200, false, false);
        const uint64_t before = g_alloc.load();
        for (const auto& d : deltas) s.llm_content(d);
        const uint64_t stream_alloc = g_alloc.load() - before;

        // The queue this wave deleted, rebuilt here only to be measured against: one node per push
        // and a copy of the payload into it. Never shipped.
        std::deque<std::string> q;
        const uint64_t before_q = g_alloc.load();
        for (const auto& d : deltas) q.push_back(d);
        const uint64_t queue_alloc = g_alloc.load() - before_q;
        q.clear();

        chk(stream_alloc == 0,
            "hand-off: a 200-delta streamed turn allocates nothing (" + std::to_string(stream_alloc) + ")");
        chk(queue_alloc > stream_alloc,
            "hand-off: the queue shape allocated per delta (" + std::to_string(queue_alloc) + " vs " + std::to_string(stream_alloc) + ")");

        // The ring's slots too: fixed words, no node, over a publish/claim cycle with a POD payload.
        SlotRing<64, uint64_t> r;
        r.publish(uint64_t{1});
        uint64_t v = 0, seq = 0;
        r.claim_one(v, &seq);
        const uint64_t before_r = g_alloc.load();
        for (int i = 0; i < 200; ++i) { r.publish_any(static_cast<uint64_t>(i)); r.claim_one(v, &seq); }
        const uint64_t ring_alloc = g_alloc.load() - before_r;
        chk(ring_alloc == 0, "ring: publish and claim allocate nothing in the steady state (" + std::to_string(ring_alloc) + ")");

        std::printf("hand-off allocations: stream %llu, queue %llu, ring %llu\n",
                    static_cast<unsigned long long>(stream_alloc), static_cast<unsigned long long>(queue_alloc),
                    static_cast<unsigned long long>(ring_alloc));
    }
    // ---- text ----
    chk(cp_width(U'a') == 1 && cp_width(U'日') == 2 && cp_width(0x301) == 0 && cp_width(U'⟁') == 1, "cp_width");
    chk(text_width("héllo") == 5 && text_width("日本") == 4 && text_width("a\tb") == 2, "text_width");
    chk(to_utf8(from_utf8("héllo ⟁ 日本")) == "héllo ⟁ 日本", "utf8 roundtrip");
    {
        const auto w = wrap("the quick brown fox", 9);
        chk(w.size() == 2 && w[0] == "the quick" && w[1] == "brown fox", "wrap words");
        const auto h = wrap("abcdefghijkl", 5);
        chk(h.size() == 3 && h[0] == "abcde" && h[2] == "kl", "wrap hard break");
        const auto c = wrap("one two three four five six", 7, 2);
        chk(c.size() == 2 && c[1].size() >= 3 && c[1].compare(c[1].size() - 3, 3, "…") == 0 && text_width(c[1]) <= 7, "wrap cut");
        bool fits = true;
        for (const auto& l : wrap("日本語のテキストを折り返す 日本語", 7)) fits = fits && text_width(l) <= 7;
        chk(fits, "wrap wide");
        chk(wrap("a\n\nb", 10).size() == 3, "wrap keeps blank lines");
        chk(clip("abcdef", 4) == "abc…" && clip("ab\ncd", 9) == "ab cd", "clip");
    }

    // ---- canvas + frame ----
    {
        Canvas c(6, 1);
        c.put(1, 0, U'日', Style{});
        c.put(2, 0, U'x', Style{});   // over the right half
        chk(c.row_text(0) == "  x", "wide overwrite");
        Frame f(20, 3);
        f.text(0, 0, "hello", Style{rgb(0xFF0000)});
        const std::string first = f.present(true);
        chk(first.find("hello") != std::string::npos && first.find("38;2;255;0;0") != std::string::npos, "frame first");
        f.text(0, 0, "hello", Style{rgb(0xFF0000)});
        chk(f.present(true).empty(), "frame unchanged = no bytes");
        f.text(0, 0, "hellO", Style{rgb(0xFF0000)});
        const std::string d = f.present(true);
        chk(d.find("\x1b[1;5H") != std::string::npos && d.find('O') != std::string::npos && d.find("hell") == std::string::npos,
            "frame diff one cell");
    }

    // ---- skin ----
    {
        const std::string p = "build/ui_test_skin.yaml";
        std::ofstream(p) << "colors:\n  banner_border: \"#123456\"\n  status_bar_bg: \"#010203\"\nspinner:\n"
                            "  thinking_verbs:\n    - \"one\"\n    - \"two\"\nbranding:\n  prompt_symbol: \"> \"\n";
        const Theme t = Theme::load(p);
        chk(t.border == rgb(0x123456) && t.status_bg == rgb(0x010203) && t.verbs.size() == 2 && t.prompt_symbol == "> ",
            "theme from skin");
        chk(Theme::load("build/no-such-skin.yaml").border == Theme{}.border, "theme defaults");
        // The shipped palette is a palette too: nobody typed it, but it paints this screen. Its
        // separator hue #7200A8 on the shipped bar #10051C measures 2.15:1 -- a glyph nobody can
        // read is the same defect a skin would have been fixed for, so the default path measures
        // itself against its own ground instead of being exempt from the pass.
        const Theme d = Theme::load("build/no-such-skin.yaml");
        chk(Theme::contrast(d.dim, d.status_bg) >= 3.0, "the default palette's separator glyph clears the floor");
        chk(Theme::contrast(d.text, d.status_bg) >= 3.0 && Theme::contrast(d.muted, d.status_bg) >= 3.0,
            "the default palette's body hues clear the floor");
        chk(Theme::contrast(d.ok, d.status_bg) >= 3.0, "the status dot is legible on the bar it sits on");
        chk(d.border == Theme{}.border && d.session_border == Theme{}.session_border &&
            d.response == Theme{}.response && d.input_rule == Theme{}.input_rule,
            "a rule keeps the skin's quiet line: the floor is for text, not for borders");
        chk(d.palette_fixed >= 1, "the shipped palette had hues to move, and says so");
        Theme twice = d;
        twice.reconcile();
        chk(twice.palette_fixed == d.palette_fixed && twice.dim == d.dim && twice.text == d.text,
            "reconcile is idempotent: a palette already clear moves nothing");
    }

    // ---- layout ----
    {
        Scene s{Theme{}};
        Info info;
        info.model = "test-model";
        info.endpoint = "http://127.0.0.1:9/v1";
        info.tools = 4;
        info.max_turns = 10;
        s.set_info(info);
        s.present(120, 30, true);
        chk(s.row_text(0).rfind("╭─┤ ⟁ ARIADNE ├┤ c++ ├", 0) == 0, "transcript title");
        chk(s.row_text(0).find("session") != std::string::npos, "sidebar at 120 cols");
        chk(s.row_text(26).rfind("╭─┤ you ├", 0) == 0 && s.row_text(28).rfind("╰", 0) == 0, "prompt box rows");
        chk(s.row_text(29).find("ready") != std::string::npos, "status row");
        chk(s.present(120, 30, true).empty(), "idle frame writes nothing");
        s.present(90, 30, true);
        chk(s.row_text(0).find("session") == std::string::npos, "no sidebar at 90 cols");
        s.present(50, 10, true);
        chk(s.row_text(5).find("needs") != std::string::npos, "too small");
    }

    // ---- the tape panel names a mid-turn fold, and stays silent without one ----
    {
        Scene s{Theme{}};
        Info info;
        info.model = "test-model";
        info.endpoint = "http://127.0.0.1:9/v1";
        info.tools = 4;
        info.max_turns = 10;
        s.set_info(info);
        TapeView tv;
        tv.on = true;
        tv.epoch = 1;
        tv.history = 6;
        tv.tape = 6;
        tv.tokens = 9000;
        tv.prompt = 9000;
        tv.reused = 4500;
        tv.visible = 40;
        tv.deferred = 4;
        tv.maze = true;
        tv.steered = 2;   // two prompts folded into the turn that is still running
        s.set_tape(tv);
        // The tape panel is a vertical budget, not a fixture: verbose opens it at 8
        // leftover rows, normal only at 21. The sidebar's value column is 22 cells, so
        // the fold rides a form that fits rather than being clipped to "2 s…".
        s.set_verbosity(Verbosity::Verbose);
        s.present(120, 30, true);
        std::string all;
        for (int y = 0; y < 30; ++y) all += s.row_text(y);
        chk(all.find("6 msgs · 2 steered") != std::string::npos,
            "the tape panel counts the prompts folded in mid-turn");
        chk(all.find("6 msgs · 2 steered…") == std::string::npos, "and says it whole, not clipped");
        s.set_verbosity(Verbosity::Normal);
        s.present(120, 46, true);
        std::string tall;
        for (int y = 0; y < 46; ++y) tall += s.row_text(y);
        chk(tall.find("6 msgs · 2 steered") != std::string::npos,
            "a tall terminal names the fold at normal verbosity too");
        TapeView clean = tv;
        clean.steered = 0;
        s.set_tape(clean);
        s.present(120, 30, true);
        std::string none;
        for (int y = 0; y < 30; ++y) none += s.row_text(y);
        chk(none.find("steered") == std::string::npos, "a turn with no fold claims no fold");
    }

    // ---- the composer's busy hint names the boundary steering lands at ----
    {
        Scene s{Theme{}};
        s.turn_begin();
        s.present(120, 30, true);
        std::string busy;
        for (int y = 24; y < 30; ++y) busy += s.row_text(y);
        chk(busy.find("type ahead — it lands at the next tool round") != std::string::npos,
            "a prompt typed mid-turn is told where it lands");
        chk(busy.find("it sends when the turn ends") == std::string::npos,
            "and not the stale promise that it waits for the turn");
        s.turn_end();
        s.present(120, 30, true);
        std::string idle;
        for (int y = 24; y < 30; ++y) idle += s.row_text(y);
        chk(idle.find("ask the maze…") != std::string::npos, "idle asks normally");
    }

    // ---- cards: tool card height is fixed; a skipped call still resolves ----
    {
        Scene s{Theme{}};
        s.turn_begin();
        s.tool_call("c1", "run_terminal", "{\"cmd\":\"ls\"}");
        s.tool_result("c1", "{\"cmd\":\"ls\",\"exit\":0,\"stdout\":\"" + std::string(5000, 'x') + "\"}");
        s.tool_call("c2", "read_file", "{\"path\":\"/x\"}");
        s.tool_result("c2", "{\"path\":\"/x\",\"content\":\"y\"}");
        const auto out = lines_of(s.drain_inline(80, false, false));
        // dynamic cards: header + args rows + result marker row + result rows + footer,
        // plus a row naming the rows the cap did not show. card 1 wraps its 5000-char
        // result to the 12-row cap and says so (17 rows); card 2 is whole, so 5.
        chk(out.size() == 17 + 1 + 5 + 1, "tool cards grow with their result and their cut row (17 + 1 + 5 + trailing)");
        const std::string tail = "─ ✓ done ┐";
        chk(out[0].rfind("┌ ▸ run_terminal ─", 0) == 0 && out[0].size() > tail.size() &&
            out[0].compare(out[0].size() - tail.size(), tail.size(), tail) == 0, "tool card header");
        s.tool_call("c3", "list_dir", "{}");
        chk(s.drain_inline(80, false, false).empty(), "unresolved card waits");
        s.turn_end();
        const std::string rest = s.drain_inline(80, false, false);
        chk(rest.find("cancelled") != std::string::npos && rest.find("turn 1") != std::string::npos, "turn end resolves");
    }

    // ---- verbosity: quiet folds a turn to a glance, verbose opens what it carried ----
    // The words themselves, before any card is drawn: three places set the level (config,
    // DD_VERBOSE, /verbose) and all three go through this one table. A value outside it is not a
    // level — it must be rejected rather than read as Normal, because DD_VERBOSE=1 used to match
    // nothing and leave the configured level alone while /verbose 1 fell through to Normal and
    // reported "verbose normal", dropping a level nobody asked to drop.
    {
        chk(is_verbosity_word("1") && verbosity_from_word("1") == Verbosity::Verbose, "1 turns verbose on");
        chk(is_verbosity_word("true") && verbosity_from_word("true") == Verbosity::Verbose, "true turns verbose on");
        chk(is_verbosity_word("on") && verbosity_from_word("on") == Verbosity::Verbose, "on turns verbose on");
        chk(is_verbosity_word("FULL") && verbosity_from_word("FULL") == Verbosity::Verbose, "an env value is read case-insensitively");
        chk(is_verbosity_word("0") && verbosity_from_word("0") == Verbosity::Minimal, "0 turns verbose off");
        chk(is_verbosity_word("false") && verbosity_from_word("false") == Verbosity::Minimal, "false turns verbose off");
        chk(is_verbosity_word("off") && verbosity_from_word("off") == Verbosity::Minimal, "off turns verbose off");
        chk(is_verbosity_word("normal") && verbosity_from_word("normal") == Verbosity::Normal, "normal is the glance");
        chk(is_verbosity_word("default") && verbosity_from_word("default") == Verbosity::Normal, "default is the glance");
        chk(!is_verbosity_word("loud"), "a word outside the table is not a level");
        chk(!is_verbosity_word(""), "an empty value is not a level: it must not override the config");
        chk(!is_verbosity_word("2"), "a number outside 0 and 1 is not a level");
        chk(verbosity_label(Verbosity::Minimal) == std::string("off")
            && verbosity_label(Verbosity::Verbose) == std::string("on"), "a kept level has a word the refusal can name");
    }

    {
        const std::string long_cmd = std::string(120, 'x');
        auto card = [&](Verbosity v) {
            Scene s{Theme{}};
            s.set_verbosity(v);
            s.turn_begin();
            s.tool_call("c1", "run_terminal", "{\"cmd\":\"ls -la " + long_cmd + "\"}");
            s.tool_result("c1", "{\"exit\":0,\"stdout\":\"" + std::string(4000, 'y') + "\"}");
            return lines_of(s.drain_inline(80, false, false));
        };
        const auto quiet = card(Verbosity::Minimal);
        const auto normal = card(Verbosity::Normal);
        const auto loud = card(Verbosity::Verbose);
        chk(quiet.size() < normal.size() && normal.size() < loud.size(), "verbosity grows a tool card");
        auto xs = [](const std::vector<std::string>& ls) {
            size_t n = 0;
            for (const auto& l : ls)
                for (char ch : l) if (ch == 'x') ++n;
            return n;
        };
        chk(xs(quiet) < xs(normal) && xs(normal) < xs(loud), "arguments widen with verbosity");
        chk(xs(loud) >= long_cmd.size(), "verbose carries the whole argument value");
        chk(loud.size() > normal.size() + 12, "verbose opens the result past the normal cap");

        auto reason = [&](Verbosity v) {
            Scene s{Theme{}};
            s.set_verbosity(v);
            s.turn_begin();
            s.llm_begin();
            for (int i = 0; i < 30; ++i) s.llm_reasoning("line " + std::to_string(i) + "\n");
            s.llm_end(LlmStats{});
            return lines_of(s.drain_inline(80, false, false));
        };
        chk(reason(Verbosity::Minimal).size() == 2, "quiet folds reasoning to its header row");
        chk(reason(Verbosity::Verbose).size() == 30 + 3, "verbose shows every reasoning line");
        chk(reason(Verbosity::Normal).size() == Scene::kReasonRows + 3, "normal keeps the tail cap");
    }

    // ---- verbose tells the truth about a result: the tool's cut, not the card's cap ----
    {
        const std::string cut = "{\"cmd\":\"grep\",\"exit\":0,\"stdout\":\"" + std::string(6000, 'x') +
                                "\",\"truncated\":true,\"output_bytes\":1009800,\"bytes_dropped\":1001800}";
        // Scene holds atomics, so it is built in place rather than returned.
        auto filled = [&](Scene& s, Verbosity v, const std::string& result) {
            s.set_verbosity(v);
            s.turn_begin();
            s.tool_call("c1", "run_terminal", "{\"cmd\":\"grep -R TODO\"}");
            s.tool_result("c1", result);
            s.turn_end();
        };
        auto frame = [](Scene& s, int cols, int rows) {
            s.present(cols, rows, true);
            std::string all;
            for (int y = 0; y < rows; ++y) all += s.row_text(y);
            return all;
        };

        Scene v{Theme{}};
        filled(v, Verbosity::Verbose, cut);
        // wide enough that the transcript column, not the sidebar, decides card width
        const auto loud = frame(v, 150, 64);
        chk(loud.find("✓ done · 1,001,800 bytes dropped") != std::string::npos, "header states the tool's dropped bytes");
        chk(loud.find("8,000 of 1,009,800 bytes reached the model · 1,001,800 dropped") != std::string::npos,
            "verbose states produced vs kept and they reconcile");
        chk(loud.find("rows past the verbose cap") != std::string::npos, "the card's own cap announces itself");
        chk(loud.find(std::string(1009800, 'x')) == std::string::npos, "a card never renders what the tool produced");

        // a folded card still carries the cut: verbosity hides payload, not truth
        Scene m{Theme{}};
        filled(m, Verbosity::Minimal, cut);
        chk(frame(m, 80, 24).find("978.3 KB dropped") != std::string::npos, "a folded card says its result was cut");

        // narrow header: the number moves to the wrapped rows, it is never clipped away.
        // Below kMinCols there is no transcript frame to read, so this is the inline path.
        Scene n{Theme{}};
        filled(n, Verbosity::Verbose, cut);
        std::string narrow;
        for (const auto& l : lines_of(n.drain_inline(44, false, false))) narrow += l + "\n";
        chk(narrow.find("✓ done ┐") != std::string::npos, "a tight header degrades to timing, not to a clipped number");
        chk(narrow.find("model · 1,001,800 dropped") != std::string::npos, "the account wraps instead of cutting mid-sentence");

        // a cut read names the lines that went and the offset a continuation starts at
        std::string body;
        for (int i = 1; i <= 300; ++i) body += "  " + std::to_string(i) + "| line\n";
        Scene f{Theme{}};
        f.set_verbosity(Verbosity::Verbose);
        f.turn_begin();
        f.tool_call("c1", "read_file", "{\"path\":\"a.cpp\"}");
        f.tool_result("c1", "{\"path\":\"a.cpp\",\"content\":\"" + body +
                     "\",\"total_lines\":1312,\"offset\":1,\"lines\":300,\"truncated\":true,\"next_offset\":301,\"lines_dropped\":1012}");
        f.turn_end();
        chk(frame(f, 150, 64).find("300 of 1,312 lines · 1,012 dropped · continue at offset 301") != std::string::npos,
            "a cut read says how many lines went and where to continue");
    }
    {   // the accounting a verbose run asks for is not on the status row otherwise
        auto status_has = [&](Verbosity v) {
            Scene s{Theme{}};
            s.set_verbosity(v);
            Info info;
            info.model = "m";
            info.endpoint = "http://127.0.0.1:9/v1";
            info.tools = 4;
            info.max_turns = 10;
            s.set_info(info);
            s.turn_begin();
            s.llm_begin();
            s.llm_content("hi");
            LlmStats st;
            st.completion_tokens = 400;
            st.cached_tokens = 2000;
            s.llm_end(st);
            s.turn_end();
            s.present(120, 30, true);
            bool acct = false, mode = false;
            for (int y = 0; y < 30; ++y) {
                const std::string t = s.row_text(y);
                if (t.find("calls ") != std::string::npos && t.find("out ") != std::string::npos && t.find("cache ") != std::string::npos) acct = true;
                if (t.find("mode ") != std::string::npos) mode = true;
            }
            return acct && mode;
        };
        chk(status_has(Verbosity::Verbose), "verbose status carries calls, out, cache, mode");
        chk(!status_has(Verbosity::Normal), "normal status carries no accounting");
    }
    {   // the todo ledger: a sidebar panel in every mode, a count on the status row in verbose
        auto with_plan = [&](Verbosity v, int cols = 120) {
            Scene s{Theme{}};
            s.set_verbosity(v);
            Info info;
            info.model = "m";
            info.endpoint = "http://127.0.0.1:9/v1";
            info.tools = 4;
            info.max_turns = 10;
            s.set_info(info);
            PlanView p;
            p.items.emplace_back(PlanView::Done, "build the panel");
            p.items.emplace_back(PlanView::Active, "wire the ephemeral prompt");
            p.open = 1;
            p.total = 2;
            s.set_plan(p);
            s.turn_begin();
            s.llm_begin();
            s.llm_content("hi");
            s.llm_end(LlmStats{});
            s.turn_end();
            s.present(cols, 30, true);
            bool panel = false, active = false, status = false;
            for (int y = 0; y < 30; ++y) {
                const std::string t = s.row_text(y);
                if (t.find("plan · 1 open of 2") != std::string::npos) panel = true;
                if (t.find("wire the ephemeral prompt") != std::string::npos && t.find("▶") != std::string::npos) active = true;
                if (t.find("plan 1/2") != std::string::npos) status = true;
            }
            return std::make_tuple(panel, active, status);
        };
        auto [panel, active, status] = with_plan(Verbosity::Verbose);
        chk(panel, "the plan panel shows in verbose");
        chk(active, "the active item carries its marker");
        chk(status, "verbose status carries the plan count");
        auto [p2, a2, s2] = with_plan(Verbosity::Normal);
        chk(p2 && a2, "the plan panel shows in normal too");
        chk(!s2, "normal status carries no plan count");
        auto [p3, a3, s3] = with_plan(Verbosity::Normal, 80);
        chk(p3 && a3, "a terminal too narrow for the sidebar still shows the plan, as a strip above the prompt");
        Scene s{Theme{}};
        s.set_verbosity(Verbosity::Verbose);
        s.present(120, 30, true);
        bool gone = true;
        for (int y = 0; y < 30; ++y)
            if (s.row_text(y).find("plan ·") != std::string::npos) gone = false;
        chk(gone, "an empty ledger draws no panel");
    }

    // ---- memory card: fixed columns, one row per memory, the skill last ----
    {
        Scene s{Theme{}};
        RecallView v;
        v.meta = "2 recalled · 1 linked · 3.1s";
        v.skill = "strata-kv-streaming-seam";
        v.rows = {{'r', 1371279, "decision:dd-context-tape-architecture", "the tape renders once", 0.49},
                  {'r', 42, "fact:x", "short", 0.51},
                  {'l', 1371273, "turn 10-20 17:29", "“list_dir include” → three headers", 1.0}};
        s.recalled(v);
        const auto out = lines_of(s.drain_inline(100, false, false));
        chk(out.size() == 7, "memory card: 3 rows + skill + box");
        chk(out[0].find("◈ mazemaker") != std::string::npos && out[0].find("2 recalled · 1 linked · 3.1s") != std::string::npos,
            "memory card header");
        const size_t l1 = out[1].find("decision:"), l2 = out[2].find("fact:x");
        chk(l1 != std::string::npos && out[1].find("#1371279") != std::string::npos &&
                out[2].find("     #42") != std::string::npos && out[3].find("↳") != std::string::npos,
            "ids right-aligned, kinds marked");
        chk(dd::ui::text_width(out[1].substr(0, l1)) == dd::ui::text_width(out[2].substr(0, l2)), "labels in one column");
        chk(out[4].find("skill_load(\"strata-kv-streaming-seam\")") != std::string::npos, "skill row");
        v.meta = std::string(200, 'm');
        s.recalled(v);
        const auto narrow = lines_of(s.drain_inline(60, false, false));
        chk(!narrow.empty() && narrow[0].find("◈ mazemaker") != std::string::npos && narrow[0].find("…") != std::string::npos,
            "a long summary is clipped, the title stays");
    }

    // ---- streamed reasoning + answer ----
    {
        Scene s{Theme{}};
        s.turn_begin();
        s.llm_begin();
        for (int i = 0; i < 40; ++i) s.llm_reasoning("step " + std::to_string(i) + " of the plan\n");
        chk(s.drain_inline(80, false, false).empty(), "live reasoning waits (inline)");
        s.present(120, 40, true);
        bool live = false;
        for (int y = 0; y < 40; ++y) live = live || s.row_text(y).find("◈ thinking") != std::string::npos;
        chk(live, "live reasoning card on screen");
        s.llm_content("The ");
        s.llm_content("answer.");
        LlmStats st;
        st.latency_ms = 1234;
        s.llm_end(st);
        const auto out = lines_of(s.drain_inline(80, false, false));
        const size_t R = Scene::kReasonRows;
        chk(out.size() == R + 3 + 4, "reason tail (capped) + answer heights");
        if (out.size() == R + 3 + 4) {
            // 40 deltas of "step N of the plan" are five word-runs each, so the estimate is ~200
            // tokens. "40" was the old count of SSE hand-offs, which is not a token count.
            chk(out[0].find("◈ thought") != std::string::npos && out[0].find("200 tok") != std::string::npos, "reason header");
            chk(out[R].find("step 39") != std::string::npos && out[1].find("step 34") != std::string::npos, "tail = newest lines");
            chk(out[R + 2].find("↑ 34 lines") != std::string::npos, "hidden-lines marker under the border");
            chk(out[R + 3].find("ARIADNE") != std::string::npos && out[R + 3].find("1.2s") != std::string::npos &&
                    out[R + 4].find("The answer.") != std::string::npos, "streamed answer card");
        }
    }

    // ---- the token number keeps growing: a run boundary never restarts it, a new call adds ----
    // Measured on the shape that broke: the counter counted SSE hand-offs, so a server that
    // batches tokens showed "3 tok" for a long answer, and every run boundary — which happens
    // whenever the render thread claims everything pending — reset the number back to 1.
    {
        auto tok_on_row = [&](const Scene& s, const std::string& needle) -> long {
            for (int y = 0; y < 30; ++y) {
                const std::string t = s.row_text(y);
                const size_t p = t.find(needle);
                if (p == std::string::npos) continue;
                size_t i = p + needle.size();
                if (t.compare(i, 3, "≈") == 0) i += 3;   // U+2248 is three bytes; a char cannot hold it
                long v = 0;
                while (i < t.size() && t[i] >= '0' && t[i] <= '9') { v = v * 10 + (t[i] - '0'); ++i; }
                // "508 tok" puts the unit at the cursor; "1.0k tok" leaves ".0k" between the
                // digits and it, since fmt_tok stops emitting digits at the decimal point.
                for (size_t off = 0; off <= 4; ++off)
                    if (t.compare(i + off, 4, " tok") == 0) return v;
            }
            return -1;
        };
        Scene s{Theme{}};
        s.set_verbosity(Verbosity::Verbose);
        Info info;
        info.model = "m";
        info.endpoint = "http://127.0.0.1:9/v1";
        info.tools = 4;
        info.max_turns = 10;
        s.set_info(info);
        s.turn_begin();
        s.llm_begin();
        long prev = 0;
        bool grew = true;
        for (int i = 0; i < 4; ++i) {   // four deltas, each claimed before the next: four run boundaries
            s.llm_content("many words in this chunk of streamed answer text");
            s.present(120, 30, true);
            const long cur = tok_on_row(s, "✎ answering · ");
            grew = grew && cur > prev;   // never a reset
            prev = cur;
        }
        chk(grew && prev > 4, "streaming: the answer's token number grows past the hand-off count (" + std::to_string(prev) + ")");
        LlmStats st;
        st.completion_tokens = 500;
        s.llm_end(st);
        s.llm_begin();
        s.llm_content("second call, first chunk");
        s.present(120, 30, true);
        const long on_row = tok_on_row(s, "✎ answering · ");
        chk(on_row > 500, "the second call ADDS to the turn's output instead of resetting it (" + std::to_string(on_row) + ")");
        s.llm_end(st);
        s.turn_end();
        s.present(120, 30, true);
        bool acct = false;
        for (int y = 0; y < 30; ++y)
            acct = acct || s.row_text(y).find("out 1.0k") != std::string::npos;
        chk(acct, "the verbose row reports the turn's total output, not the last call's");
    }
    {   // the tail is fixed height even for one line; hide = one row; full = every line
        auto run = [](ReasonView v, int lines) {
            Scene s{Theme{}};
            s.set_reason_view(v);
            s.turn_begin();
            s.llm_begin();
            for (int i = 0; i < lines; ++i) s.llm_reasoning("line " + std::to_string(i) + "\n");
            s.llm_end(LlmStats{});
            return lines_of(s.drain_inline(80, false, false));
        };
        chk(run(ReasonView::Tail, 1).size() == 1 + 3, "tail grows: 1 line = 1 row");
        chk(run(ReasonView::Tail, 30).size() == Scene::kReasonRows + 3, "tail caps at kReasonRows");
        const auto hide = run(ReasonView::Hide, 30);
        chk(hide.size() == 2 && hide[0].find("◈ thought") != std::string::npos && hide[1].empty(), "hide = one row");
        chk(run(ReasonView::Full, 30).size() == 30 + 3, "full = every line");
    }

    // ---- a re-sent call is named on screen, not mysterious (W4) ----
    // The sidebar\'s value column is 22 cells, so the "last" row ranks its forms
    // instead of appending them: a clipped "2 se…" or "800 c…" would report a fact
    // without saying what it was. Both orders are asserted, so the ranking is proven
    // rather than assumed.
    {
        Scene s{Theme{}};
        Info info;
        info.model = "qwen3.8-flash-next";
        info.n_ctx = 4096;
        info.tools = 34;
        s.set_info(info);
        s.turn_begin();
        s.llm_begin();
        s.llm_content("The answer.");
        LlmStats st;
        st.latency_ms = 1900;
        st.gen_tps = 31.5;
        st.prompt_tokens = 1200;
        st.completion_tokens = 60;
        st.cached_tokens = 800;
        st.attempts = 2;   // the first send never reached an answer
        s.llm_end(st);

        // verbose opens the engine panel at a shorter height; the send count rides it
        s.set_verbosity(Verbosity::Verbose);
        s.present(160, 30, true);
        std::string wide;
        for (int y = 0; y < 30; ++y) wide += s.row_text(y);
        chk(wide.find("last") != std::string::npos && wide.find("60 tok · 1.9s · ×2") != std::string::npos,
            "engine panel names the re-send");
        chk(wide.find("sent 2") != std::string::npos, "the status row carries the send count");
        chk(wide.find("cache 800") != std::string::npos, "the cache readout rides the status row");

        // one send: no marker. The row cannot hold tok + latency + cache in 22 cells,
        // so the cache segment is dropped whole rather than clipped to "800 c…" — the
        // spelled-out readout belongs to the status row, which has the room.
        LlmStats once;
        once.latency_ms = 1900;
        once.gen_tps = 31.5;
        once.prompt_tokens = 1200;
        once.completion_tokens = 60;
        once.cached_tokens = 800;
        once.attempts = 1;
        s.llm_end(once);
        s.present(160, 30, true);
        std::string again;
        for (int y = 0; y < 30; ++y) again += s.row_text(y);
        chk(again.find("×") == std::string::npos, "a single send is not marked");
        chk(again.find("60 tok · 1.9s") != std::string::npos, "the bare call reads without a marker");
        chk(again.find("800 c") == std::string::npos, "no cache segment is clipped mid-word");
        chk(again.find("cache 800") != std::string::npos, "cache still reads on the status row");
    }

    chk(g_early.width == 16, "text_width from a static initializer (lazy kernel pick)");

    // ---- decode: well-formed UTF-8 only (RFC 3629) ----
    {
        auto cps = [](const std::string& b) { return from_utf8(b); };
        chk(cps("\xC0\xAF") == std::u32string(2, U'\uFFFD'), "overlong '/' -> U+FFFD per byte");
        chk(cps("\xED\xA0\xBD") == std::u32string(3, U'\uFFFD'), "surrogate -> U+FFFD per byte");
        chk(cps("\xF4\x90\x80\x80") == std::u32string(4, U'\uFFFD'), "U+110000 (the repaint sentinel) is never decoded");
        chk(cps("\xF4\x8F\xBF\xBF") == std::u32string(1, char32_t(0x10FFFF)), "U+10FFFF decodes");
        chk(cps("\xE0\x80\x80") == std::u32string(3, U'\uFFFD'), "overlong 3-byte");
        chk(cps("\xF0\x9F\x98\x80") == std::u32string(1, char32_t(0x1F600)), "emoji decodes");
        chk(cps("a\xE2\x82") == std::u32string{U'a', U'\uFFFD', U'\uFFFD'}, "cut sequence at the end");
        Canvas c(6, 1);
        c.text(0, 0, "\xF4\x90\x80\x80", Style{});
        chk(c.at(0, 0).ch == U'\uFFFD', "no sentinel glyph reaches a cell");
    }

    // ---- the sidebar's tools title is a middle dot, not mojibake ----
    {
        Scene s{Theme{}};
        Info info;
        info.model = "m";
        info.endpoint = "http://127.0.0.1:9/v1";
        info.tools = 7;
        info.max_turns = 4;
        s.set_info(info);
        s.present(120, 30, true);
        bool title = false, mojibake = false;
        for (int y = 0; y < 30; ++y) {
            const std::string r = s.row_text(y);
            title = title || r.find("tools \xC2\xB7 7") != std::string::npos;
            mojibake = mojibake || r.find("\xC3\x82") != std::string::npos;
        }
        chk(title, "sidebar title reads 'tools · 7'");
        chk(!mojibake, "no double-encoded characters on screen");
    }

    // ---- a long list of calls squeezes nothing: the turns box stays, the tools box scrolls inside ----
    {
        Scene s{Theme{}};
        Info info;
        info.model = "m";
        info.endpoint = "http://127.0.0.1:9/v1";
        info.tools = 7;
        info.max_turns = 80;
        s.set_info(info);
        for (int t = 0; t < 2; ++t) {
            s.turn_begin();
            for (int i = 0; i < 30; ++i) {
                const std::string id = "c" + std::to_string(t) + "_" + std::to_string(i);
                s.tool_call(id, "tool_" + std::to_string(t) + "_" + std::to_string(i), "{}");
                s.tool_result(id, "{}");
            }
            s.turn_end();
        }
        auto find_row = [&](const std::string& what) {
            for (int y = 0; y < 30; ++y)
                if (s.row_text(y).find(what) != std::string::npos) return y;
            return -1;
        };
        s.present(120, 30, true);
        chk(find_row("\xE2\x94\xA4 turns \xE2\x94\x9C") >= 0, "sixty calls do not squeeze the turns box out");
        const int ty = find_row("tools \xC2\xB7 7");
        chk(ty >= 0, "the tools box is still there beside it");
        chk(s.row_text(ty).find("\xE2\x86\x93") != std::string::npos, "the tools box says more lies below it");
        chk(s.row_text(ty).find("\xE2\x86\x91") == std::string::npos, "and nothing above, at the newest");
        chk(find_row("\xE2\x9C\x93 tool_1_29") >= 0 && find_row("\xE2\x9C\x93 tool_0_0") < 0, "newest first; the oldest is past the edge");
        chk(s.over_tools(110, ty + 1) && !s.over_tools(5, ty + 1), "a wheel over the box is the box's, over the transcript is not");
        s.scroll_tools(4);
        s.present(120, 30, true);
        chk(s.row_text(find_row("tools \xC2\xB7 7")).find("\xE2\x86\x91 4") != std::string::npos, "scrolled four calls the box says so");
        chk(find_row("\xE2\x9C\x93 tool_1_29") < 0 && find_row("\xE2\x9C\x93 tool_1_25") >= 0, "the list moved four calls older");
        chk(find_row("\xE2\x94\xA4 turns \xE2\x94\x9C") >= 0, "the turns box stays through the scroll");
        s.scroll_tools(10000);
        s.present(120, 30, true);
        chk(find_row("\xE2\x9C\x93 tool_0_0") >= 0, "scrolled to the end the oldest call shows");
        chk(s.row_text(find_row("tools \xC2\xB7 7")).find("\xE2\x86\x93") == std::string::npos, "and nothing lies below it");
        s.scroll_tools(-10000);
        s.present(120, 30, true);
        chk(find_row("\xE2\x9C\x93 tool_1_29") >= 0, "back at the top the newest call shows");
    }

    // ---- /reasoning's vocabulary: one table, so the palette and the handler cannot drift ----
    {
        // A bare or mistyped /reasoning must never be read as a view word: it reports, or goes to
        // the level handler. It used to fall through to /effort, so asking about the pane moved
        // the effort knob and answered with another command's vocabulary.
        chk(!is_reason_view_word(""), "a bare /reasoning is not a view word");
        chk(!is_reason_view_word("nonsense"), "an unknown word is not a view word");
        chk(!is_reason_view_word("high"), "an effort level is not a view word");
        chk(is_reason_view_word("show") && is_reason_view_word("tail") &&
            is_reason_view_word("full") && is_reason_view_word("hide"),
            "every word the palette offers is one the handler accepts");
        chk(reason_view_from_word("full") == ReasonView::Full &&
            reason_view_from_word("hide") == ReasonView::Hide &&
            reason_view_from_word("show") == ReasonView::Tail &&
            reason_view_from_word("tail") == ReasonView::Tail,
            "each word maps to exactly one view");
        // The label is what the operator reads back, and the read-back must describe the state set.
        Scene s{Theme{}};
        for (const char* w : kReasonViewWords) {
            s.set_reason_view(reason_view_from_word(w));
            chk(std::string(reason_view_label(s.reason_view())) == reason_view_label(reason_view_from_word(w)),
                std::string(w) + " reads back as itself");
        }
    }

    // ---- a streaming card keeps at most 512 KiB, cut at a character ----
    {
        Scene s{Theme{}};
        s.set_reason_view(ReasonView::Full);
        s.turn_begin();
        s.llm_begin();
        s.llm_reasoning("HEADMARK ");
        std::string chunk;
        for (int i = 0; i < 30; ++i) chunk += "\xE2\x82\xAC";   // 3-byte characters: the cut lands inside one
        // Rendering as dd_tui does: a frame every hundred deltas. The stream buffer is fixed, so a
        // renderer that keeps up is the only way a long stream survives whole — and keeping up means
        // draining, not buffering.
        for (int i = 0; i < 7000; ++i) {
            s.llm_reasoning(chunk);
            if (i % 100 == 99) s.present(120, 40, true);
        }
        s.llm_reasoning(" TAILMARK");
        s.present(120, 40, true);
        chk(s.dropped_live_bytes() == 0, "a renderer that keeps up loses nothing");
        const size_t held = s.live_text_bytes();
        chk(held <= (512u << 10) && held > (400u << 10), "live reasoning bounded (" + std::to_string(held) + " bytes)");
        s.scroll(10000000);
        s.present(120, 40, true);
        bool head = false, repl = false, euro = false;
        for (int y = 0; y < 40; ++y) {
            const std::string r = s.row_text(y);
            head = head || r.find("HEADMARK") != std::string::npos;
            repl = repl || r.find("\xEF\xBF\xBD") != std::string::npos;
            euro = euro || r.find("\xE2\x82\xAC") != std::string::npos;
        }
        chk(!head && !repl && euro, "oldest bytes dropped, first kept character is whole");
        s.scroll(-10000000);
        s.present(120, 40, true);
        bool tail = false;
        for (int y = 0; y < 40; ++y) tail = tail || s.row_text(y).find("TAILMARK") != std::string::npos;
        chk(tail, "the newest text is kept");
    }
    {
        Scene s{Theme{}};
        s.turn_begin();
        s.llm_begin();
        const std::string chunk(4096, 'w');
        for (int i = 0; i < 200; ++i) {
            s.llm_content(chunk);
            if (i % 20 == 19) s.present(120, 40, true);   // the renderer keeps up
        }
        s.present(120, 40, true);
        chk(s.live_text_bytes() <= (512u << 10) && s.live_text_bytes() > (400u << 10), "live answer bounded");
        chk(s.dropped_live_bytes() == 0, "an answer streamed to a live renderer loses no bytes");

        // The renderer stalled: the buffer fills and the rest of the stream is refused, which is a
        // number the reply names. Nothing is overwritten out from under the render thread.
        Scene stalled{Theme{}};
        stalled.turn_begin();
        stalled.llm_begin();
        for (int i = 0; i < 200; ++i) stalled.llm_content(chunk);   // 800 KiB, nobody draining
        chk(stalled.dropped_live_bytes() > 0, "a stalled renderer's refusals are counted");
        // The buffer holds exactly its capacity and the rest is refused: nothing is overwritten out
        // from under the render thread, and the shortfall is a number, not a vanishing.
        chk(stalled.dropped_live_bytes() == (800u << 10) - (512u << 10),
            "the buffer fills at its capacity and the rest is counted (" + std::to_string(stalled.dropped_live_bytes()) + " refused)");
        stalled.present(120, 40, true);
        chk(stalled.live_text_bytes() <= (512u << 10), "the held text stays bounded");
    }

    // ---- 1 thread vs 8: same frames, byte for byte ----
    {
        auto build = [] {
            auto s = std::make_unique<Scene>(Theme{});
            for (int i = 0; i < 60; ++i) {
                s->note("question " + std::to_string(i) + " " + std::string(static_cast<size_t>(i * 7), 'q'));
                s->say("answer " + std::to_string(i) + " — " + std::string(static_cast<size_t>(i * 13), 'a') + " 日本");
            }
            return s;
        };
        auto s1 = build(), s8 = build();
#ifdef _OPENMP
        omp_set_num_threads(1);
        const std::string a = s1->present(240, 80, true);
        omp_set_num_threads(8);
        const std::string b = s8->present(240, 80, true);
#else
        // No libgomp in this build (TSan cannot model it), so both scenes render sequentially:
        // the byte-for-byte comparison below is then serial-vs-serial, which still catches a
        // frame that depends on a thread's arrival order but not a thread-count bug. The shipped
        // -fopenmp run is where the parallel claim is actually falsified.
        const std::string a = s1->present(240, 80, true);
        const std::string b = s8->present(240, 80, true);
#endif
        bool rows = a == b;
        for (int y = 0; y < 80; ++y) rows = rows && s1->row_text(y) == s8->row_text(y);
        chk(rows, "present matches across thread counts");
    }

    // ---- code colour: painting never rewrites what the model wrote ----
    {
        const Theme th;
        // A row's spans must concatenate back to the row exactly. Colour may split a row; it
        // may never edit one. Wide glyphs, tabs and diff markers are in the set because those
        // are where a naive splitter drops or duplicates a column.
        const std::vector<std::string> rows = {
            "plain prose row",
            "- a bullet, not a deletion",
            "+int x = 42;",
            "@@ -1,3 +1,4 @@",
            "--- a/src/ui.cpp",
            "*** Add File: src/new.cpp",
            "\tint x = 42;  // a comment",
            "日本 + not a diff marker",
            "",
        };
        for (const auto& row : rows) {
            CodeCtx ctx;
            ctx.diff_body = looks_like_diff(row);
            std::string back;
            for (const auto& s : highlight_row(row, th, ctx)) back += s.text;
            chk(back == row, "spans concatenate back to the row: " + row);
        }

        // +/- colouring is honest only when the body carries diff markers at all. A prose '-'
        // bullet stays the body's own colour: calling a bullet a deletion is the lie the state
        // machine exists to refuse.
        const std::string prose_body = "notes:\n- a bullet\n+ a plus sign in prose";
        chk(!looks_like_diff(prose_body), "prose is not a diff body");
        CodeCtx prose;
        prose.diff_body = looks_like_diff(prose_body);
        const auto bullet = highlight_row("- a bullet, not a deletion", th, prose);
        chk(bullet.size() == 1 && bullet[0].text == "- a bullet, not a deletion" &&
            bullet[0].st == Style{th.text}, "a prose bullet is not coloured as a deletion");

        // A real diff body does colour: the marker first, then the row's own code.
        chk(looks_like_diff("--- a/src/ui.cpp\\n+++ b/src/ui.cpp\\n@@ -1,3 +1,4 @@"), "a diff body is recognised");
        // Evidence inside a fence belongs to that fence. A message whose ```diff block carries a
        // hunk header must still read its prose after the block as prose — otherwise the fence
        // paints the whole message, which is the lie this heuristic exists to refuse. A patch
        // pasted with no fence at all keeps colouring, because there nothing claimed the rows.
        const std::string fenced_mixed =
            "```diff\\n@@ -1,3 +1,4 @@ reconcile\\n+added line\\n```\\n- a prose dash\\n+ a prose plus\\n";
        chk(!looks_like_diff(fenced_mixed), "a hunk header inside a fence is not body-wide evidence");
        CodeCtx fm;
        fm.diff_body = looks_like_diff(fenced_mixed);
        const auto dash = highlight_row("- a prose dash is not a deletion", th, fm);
        chk(dash.size() == 1 && dash[0].st == Style{th.text}, "prose after a closed fence stays prose");
        chk(looks_like_diff("--- a/src/ui.cpp\\n+++ b/src/ui.cpp\\n@@ -1,3 +1,4 @@\\n-int x;\\n+int y;"),
            "an unfenced patch is still recognised");
        // A hunk header claims a fixed number of rows, and inside that claim a row colours even
        // when its head is not code — a deleted `old value` is still a deletion. Past the claim
        // the same marker is prose: the contract is what scopes the colour, not the body.
        CodeCtx hc;
        hc.diff_body = true;
        highlight_row("@@ -1,2 +1,2 @@ context", th, hc);
        chk(hc.hunk_rows == 4, "a hunk header claims b + c rows");
        const auto claimed = highlight_row("-old value", th, hc);
        chk(claimed.size() >= 1 && claimed[0].st.bg == th.diff_del_bg,
            "a row inside the hunk's claim colours without a code head");
        chk(hc.hunk_rows == 3, "a claimed row spends one row of the contract");
        highlight_row(" uncontextual line", th, hc);
        chk(hc.hunk_rows == 2, "a context row belongs to the hunk and stays plain text");
        highlight_row("-second value", th, hc);
        highlight_row("+third value", th, hc);
        chk(hc.hunk_rows == 0, "the claim is spent");
        const auto after = highlight_row("- a prose dash after the patch", th, hc);
        chk(after.size() == 1 && after[0].st == Style{th.text},
            "prose past the hunk's claim stays prose");
        CodeCtx bad;
        bad.diff_body = true;
        highlight_row("@ -1 +1 @", th, bad);
        chk(bad.hunk_rows == 0, "a malformed hunk header claims nothing");
        CodeCtx nf;
        nf.diff_body = true;
        highlight_row("@@ -0,0 +1,3 @@ new file", th, nf);
        chk(nf.hunk_rows == 3, "a new-file hunk claims its added rows and nothing else");
        CodeCtx huge;
        huge.diff_body = true;
        highlight_row("@@ -1,999999 +1,999999 @@ inflated", th, huge);
        chk(huge.hunk_rows <= 8192, "an inflated claim is capped, not trusted");
        const std::string re_opened = "```diff\\n@@ -1,3 +1,4 @@\\n```\\n```diff\\n+int z = 1;\\n";
        chk(!looks_like_diff(re_opened), "a re-opened fence still owns its rows");
        CodeCtx d;
        d.diff_body = true;
        const auto add = highlight_row("+int x = 42;", th, d);
        chk(add.size() >= 1 && add[0].text == "+" && add[0].st.fg == th.diff_add &&
            add[0].st.bg == th.diff_add_bg, "an added row carries the add stripe");
        CodeCtx d2;
        d2.diff_body = true;
        const auto del = highlight_row("-int y = 1;", th, d2);
        chk(del.size() >= 1 && del[0].st.fg == th.diff_del && del[0].st.bg == th.diff_del_bg,
            "a removed row carries the delete stripe");
        CodeCtx d3;
        d3.diff_body = true;
        const auto hunk = highlight_row("@@ -1,3 +1,4 @@", th, d3);
        chk(hunk.size() == 1 && hunk[0].st.fg == th.diff_hunk && hunk[0].st.bg == th.diff_hunk_bg,
            "a hunk row reads as a hunk");

        // A fence is state across the rows of one body: it opens, colours, and closes.
        CodeCtx f;
        chk(highlight_row("```cpp", th, f).size() == 1 && f.fence, "a fence row opens the fence");
        const auto inner = highlight_row("int x = 42;  // comment", th, f);
        std::string back;
        for (const auto& s : inner) back += s.text;
        chk(back == "int x = 42;  // comment" && inner.size() >= 2, "a fenced row is split, not rewritten");
        highlight_row("```", th, f);
        chk(!f.fence, "the closing fence closes the fence");

        // paint_row reports what it painted, and a painted row reads back as the row. A row
        // clamped by a narrow box still starts with its marker: a stripe cut short reads as a
        // line that ended, which is a claim about the file that is not true.
        Canvas wide(60, 3);
        CodeCtx p;
        p.diff_body = true;
        const int used = paint_row(wide, 2, 1, "+int x = 42;", th, p, Style{th.text}, 58);
        chk(used == text_width("+int x = 42;"), "paint_row reports the columns it used");
        chk(wide.row_text(1) == "  +int x = 42;", "a painted row reads back exactly");
        Canvas narrow(12, 3);
        CodeCtx q;
        q.diff_body = true;
        paint_row(narrow, 1, 1, "+long row that does not fit", th, q, Style{th.text}, 10);
        chk(narrow.row_text(1).find("+") == 1, "a clamped row still starts with its marker");
    }

    // ---- the fence and the patch are one state machine, not two ----
    {
        const Theme th;
        const GrammarSet set = GrammarSet::load({std::filesystem::path("skills")});
        chk(set.find("diff") != nullptr, "patch rows are a language a file defines");

        // A patch body that carries a fenced block: inside the fence the named language owns the
        // row, so a python '-' is punctuation and `def` is python's keyword — a diff heuristic
        // cannot swallow the block it sits in.
        CodeCtx m;
        m.set = &set;
        m.diff_body = true;
        highlight_row("```python", th, m);
        chk(m.fence && m.lang && m.lang->name == "python", "a fence opens inside a diff body");
        const auto minus = highlight_row("- value = 1", th, m);
        chk(std::all_of(minus.begin(), minus.end(),
                        [&](const Span& s) { return s.st.bg == kNone; }),
            "a minus inside another language paints no deletion ground");
        chk(std::any_of(minus.begin(), minus.end(),
                        [&](const Span& s) { return s.text == "def" || s.text == "value"; }),
            "a row inside a fence is split by the fence's language, not read as a patch row");
        const auto defrow = highlight_row("def reconcile(self, skin):", th, m);
        chk(std::any_of(defrow.begin(), defrow.end(),
                        [&](const Span& s) { return s.text == "def" && s.st.fg != th.text; }),
            "the fenced language's keywords are coloured, not the diff's");

        // A ```diff fence colours patch rows even where the body never looked like a patch: the
        // fence is the authority, and it says so in the file, not in a table compiled into C++.
        CodeCtx d;
        d.set = &set;
        highlight_row("```diff", th, d);
        chk(d.fence && d.lang && d.lang->diff_rules, "a patch fence declares itself through the file");
        const auto add = highlight_row("+int x = 42;", th, d);
        chk(add.size() >= 1 && add.front().st.fg == th.diff_add && add.front().st.bg == th.diff_add_bg,
            "a patch fence colours an addition");
        const auto del = highlight_row("-int x = 41;", th, d);
        chk(del.size() >= 1 && del.front().st.bg == th.diff_del_bg, "a patch fence colours a deletion");
        const auto hunk = highlight_row("@@ -1,3 +1,4 @@ reconcile", th, d);
        chk(hunk.size() == 1 && hunk[0].st.bg == th.diff_hunk_bg, "a patch fence colours a hunk");
        highlight_row("```", th, d);
        chk(!d.fence, "the closing fence closes a patch fence");

        // A tagged fence re-opens. A model that forgets to close one cannot hold the block open
        // and colour every following row as code — the fence's identity must survive the reply.
        CodeCtx r;
        r.set = &set;
        highlight_row("```cpp", th, r);
        highlight_row("int x = 1;", th, r);
        highlight_row("```python", th, r);
        chk(r.fence && r.lang && r.lang->name == "python", "a tagged fence re-opens with its own language");
        const auto py = highlight_row("def load(path):  # read it", th, r);
        chk(std::any_of(py.begin(), py.end(),
                        [&](const Span& s) { return s.text == "def" && s.st.fg != th.text; }),
            "the reopened fence colours the language it names");
        std::string back;
        for (const auto& s : py) back += s.text;
        chk(back == "def load(path):  # read it", "re-opening still never rewrites the row");

        // paint_row takes the stripe ground from what the row was painted as: an uncoloured row
        // gets no ground at all, and a coloured marker gets exactly its own hue.
        Canvas c1(40, 2);
        CodeCtx p1;
        p1.set = &set;
        p1.diff_body = true;
        highlight_row("```python", th, p1);
        paint_row(c1, 1, 1, "- value = 1", th, p1, Style{th.text}, 38);
        chk(c1.row_text(1) == " - value = 1", "a painted minus inside python reads back exactly");
        chk(c1.at(1, 1).st.bg == kNone, "no ground is painted under an uncoloured marker");
        Canvas c2(40, 2);
        CodeCtx p2;
        p2.set = &set;
        highlight_row("```diff", th, p2);
        paint_row(c2, 1, 1, "-int x = 41;", th, p2, Style{th.text}, 38);
        chk(c2.at(1, 1).st.bg == th.diff_del_bg, "the painted marker carries its own ground");
    }

    // ---- a heuristic may only fire on a row nobody claimed ----
    {
        const Theme th;
        const GrammarSet set = GrammarSet::load({std::filesystem::path("skills")});
        CodeCtx c;
        c.set = &set;
        chk(!looks_like_diff("```python"), "a fence is not a patch row");
        chk(!looks_like_diff("- item one"), "a bare minus bullet is not a patch row");
        chk(!looks_like_diff("+ def load"), "an identifier alone is prose, not a patch row");
        chk(looks_like_diff("+def load(path):"), "a plus with code at its head is a patch row");
        chk(looks_like_diff("@@ -12,7 +12,9 @@ void reconcile()"), "a hunk header is a patch row");
    }

    // ---- code colouring from data: a fence names a language, a file defines it ----
    {
        const Theme th;
        const GrammarSet set = GrammarSet::from_yaml(
            "language: fizz\nkeywords: [widget]\nline_comment: [\"@\"]\n"
            "colors: {keyword: \"#00FF00\"}\n", "test");
        chk(set.find("fizz") != nullptr, "a defined language is found by name");
        chk(set.find(" FIZZ ") != nullptr, "a fence tag is matched case-insensitively");
        chk(set.find("fizzbuzz") == nullptr, "a tag no file defines resolves to nothing");

        // Colour may never rewrite what the model wrote: the runs, joined with no separator, are
        // the row byte for byte — fence row, body row, mid-line comment marker, closing fence.
        CodeCtx c;
        c.set = &set;
        std::vector<Span> all;
        for (const std::string& row : {std::string("```fizz"), std::string("widget = 3 @ note"),
                                       std::string("```")}) {
            const auto spans = highlight_row(row, th, c);
            std::string back;
            for (const auto& s : spans) back += s.text;
            chk(back == row, "spans concatenate back to the row exactly");
            all.insert(all.end(), spans.begin(), spans.end());
        }
        chk(std::any_of(all.begin(), all.end(),
                        [&](const Span& s) { return s.text == "widget" && s.st.fg != th.text; }),
            "a keyword a file declares is coloured");
        chk(std::any_of(all.begin(), all.end(),
                        [&](const Span& s) {
                            return s.text.find("@ note") != std::string::npos && s.st.fg != th.text;
                        }),
            "a comment marker a file declares is coloured");

        // An unknown tag keeps the shared facts and claims nothing: `widget` is a keyword in no
        // language the harness knows, so it reads as body text, not as this file's keyword.
        CodeCtx u;
        u.set = &set;
        highlight_row("```fizzbuzz", th, u);
        const auto unknown = highlight_row("widget = 3", th, u);
        // Numbers stay coloured under an unknown tag — they are facts in every language. Only the
        // claim no file made is refused: `widget` reads as body text, not as fizz's keyword.
        const auto kw = std::find_if(unknown.begin(), unknown.end(),
                                     [&](const Span& s) { return s.text == "widget"; });
        chk(kw != unknown.end() && kw->st.fg == th.text, "an unknown tag colours no keyword");
        chk(std::any_of(unknown.begin(), unknown.end(),
                        [&](const Span& s) { return s.text == "3" && s.st.fg != th.text; }),
            "an unknown tag still colours the facts every language shares");
        chk(u.fence == true, "an open fence stays open until its own marker");
        highlight_row("```", th, u);
        chk(u.fence == false && c.fence == false, "the fence state closes on its own marker");

        // Order of authority: skin, then language file, then palette default.
        const std::string dir = "/tmp/dd_ui_test_colors";
        std::filesystem::create_directories(dir);
        auto write = [&](const std::string& name, const std::string& body) {
            const std::string p = dir + "/" + name;
            std::ofstream f(p);
            f << body;
            f.close();
            return p;
        };
        const std::string skin_path = write("orange.yaml",
                                            "colors:\n  code_keyword: \"#FF7A00\"\n  status_bar_bg: \"#0B0714\"\n");
        const Theme sk = Theme::load(skin_path);
        chk(sk.skin_set(CodeTok::Keyword), "the skin's own code line is recorded as the skin's");
        chk(sk.code_kw == rgb(0xFF7A00), "a readable skin hue survives the contrast pass");
        // A ground is not a foreground: the pass moves hues painted *on* a ground, never the ground
        // itself. Only the defaults this ground cannot show move — and the count says how many.
        chk(sk.status_bg == rgb(0x0B0714), "the skin's ground is never moved");
        chk(sk.palette_fixed == 0 || Theme::contrast(sk.text, sk.status_bg) >= 3.0,
            "whatever moved, moved to something readable");
        const Grammar* fizz = set.find("fizz");
        chk(fizz && code_color(sk, *fizz, CodeTok::Keyword, sk.code_kw) == rgb(0xFF7A00),
            "a skin's hue outranks a language file's hue");
        chk(code_color(Theme{}, *fizz, CodeTok::Keyword, Theme{}.code_kw) == rgb(0x00FF00),
            "with no skin line the language file's hue wins");
        chk(generic_grammar().keywords.empty(), "the untagged grammar claims no keyword");

        // white on white: a hue and a ground that are the same colour paint nothing. The skin
        // asked for those colours; it did not ask for the code to be invisible.
        const std::string white_path = write("white-on-white.yaml",
                                             "colors:\n  banner_text: \"#FFFFFF\"\n  status_bar_bg: \"#FFFFFF\"\n"
                                             "  code_keyword: \"#FFFFFF\"\n  diff_add: \"#FFFFFF\"\n  diff_add_bg: \"#FFFFFF\"\n");
        const Theme w = Theme::load(white_path);
        chk(Theme::contrast(rgb(0xFFFFFF), rgb(0xFFFFFF)) == 1.0, "identical colours contrast at 1");
        chk(w.palette_fixed > 0, "an unreadable palette is reconciled, and the count says so");
        chk(Theme::contrast(w.code_kw, w.status_bg) >= 3.0, "a moved hue reads on the skin's ground");
        chk(w.code_kw != rgb(0xFFFFFF), "white on white stops being white");
        chk(w.diff_add_bg == kNone || Theme::contrast(w.diff_add, w.diff_add_bg) >= 4.5,
            "a stripe keeps a readable colour or drops its ground");
        chk(w.luminance(w.code_kw) < w.luminance(w.status_bg), "a hue moved on a light ground went dark");
        // Kinds stay distinguishable. A hue with no chroma nudged toward the ground's pole lands on
        // one grey for every kind that cannot be shown, so the block colours as a single run -- the
        // thing highlighting exists to prevent. An achromatic claim therefore falls back to the
        // palette's hue for that kind; a chromatic claim keeps its hue and loses only luminance.
        chk(w.code_kw != w.code_ty && w.code_ty != w.code_cmt,
            "an achromatic skin still paints keyword, type and comment as different hues");
        chk(Theme::contrast(w.code_cmt, w.status_bg) >= 3.0, "the fallback hue reads on the ground too");
        // The status bar paints a ground of its own, so the hues drawn on it are measured against it:
        // a dot this skin cannot show is not a status.
        chk(Theme::contrast(w.ok, w.status_bg) >= 3.0, "a status-bar hue a white ground hides is moved");
        chk(w.diff_add_bg == kNone, "a stripe whose fill is the colour of the ground is dropped");

        // A grammar file can make the same mistake a skin makes. Its hues are measured against the
        // ground the skin paints, before any of them is ever drawn — and the move is counted.
        GrammarSet bright = GrammarSet::from_yaml(
            "language: fizz\nkeywords: [widget]\ncolors: {keyword: \"#FFFFFF\"}\n", "test");
        chk(bright.languages() == 1, "one file, one language");
        bright.reconcile(w);
        chk(bright.moved_hues() == 1, "a hue a skin cannot show is moved, and counted");
        chk(Theme::contrast(bright.find("fizz")->color(CodeTok::Keyword, 0), w.status_bg) >= 3.0,
            "a moved grammar hue reads on the skin's ground");
        chk(bright.find("fizz")->keywords.size() == 1, "moving a hue does not touch the rules");
        chk(bright.find("fizz")->color(CodeTok::Keyword, 0) == w.code_kw,
            "an achromatic grammar hue falls back to the theme's hue for that kind");
        // A hue with chroma keeps it: the ladder moves toward the ground's opposite pole, so the
        // channels stay in proportion and only luminance gives way.
        GrammarSet warm = GrammarSet::from_yaml(
            "language: fizz\nkeywords: [widget]\ncolors: {keyword: \"#FF7A00\"}\n", "test");
        warm.reconcile(w);
        const uint32_t moved = warm.find("fizz")->color(CodeTok::Keyword, 0);
        chk(Theme::contrast(moved, w.status_bg) >= 3.0, "a chromatic grammar hue reads after the move");
        chk(((moved >> 16) & 0xFF) > ((moved >> 8) & 0xFF), "a chromatic hue moves by losing luminance, not hue");

        // The disk path, not just the string path. A skill directory under a root is walked and
        // indexed, in the shapes this YAML subset actually parses: `languages:` as a map keyed by
        // language name, a list on one line and as a block list, an inline map, a declared-none
        // list, and a language named only by its key. A block list of inline maps is not one of
        // them (`- language: x` parses as the string "language: x"), and a wrapped inline list
        // swallows the lines after it, so neither is a shape a skill file may write.
        const std::string root = dir + "/skills/demo";
        std::filesystem::create_directories(root);
        std::ofstream(root + "/highlight.yaml")
            << "languages:\n"
               "  grox:\n"
               "    keywords: [glorp, frint]\n"
               "    line_comment: [\"//\"]\n"
               "    block_comment: {open: \"/*\", close: \"*/\"}\n"
               "  plum:\n"
               "    keywords:\n"
               "      - plum\n"
               "      - prune\n"
               "    quotes: ['\"']\n"
               "    line_comment: []\n"
               "  moss:\n"
               "    types:\n"
               "      - Frond\n";
        const GrammarSet disk = GrammarSet::load({std::filesystem::path(root)});
        chk(disk.languages() == 3, "one file may define several languages");
        chk(disk.find("grox") != nullptr, "a file under a root is found by walking, not by name");
        chk(disk.find("grox")->keywords.size() == 2, "a keyword list on one line is a list");
        chk(disk.find("grox")->block_comment.size() == 1, "a block comment pair written inline is a pair");
        chk(disk.find("plum")->line_comment.empty(), "a declared-none line comment is none, not a prefix that matches every row");
        chk(disk.find("plum")->keywords.size() == 2, "a keyword list written as a block list is a list");
        chk(disk.find("moss") != nullptr && disk.find("moss")->types.size() == 1,
            "a language named by its key alone is still a language");
        chk(disk.sources().size() == 1, "every file that was read is named");

    }

    // ---- the grammar file this repo ships is read by the same loader as any operator's ----
    // A shipped default is data, not a table compiled in: the loader that reads an operator's
    // highlight.yaml is the only one that reads this one, so the file is exercised end to end and
    // its edge cases (a declared-none comment, one quote delimiter, an alias) are the file's own.
    {
        const Theme th;
        const GrammarSet shipped = GrammarSet::load({"skills"});
        chk(shipped.languages() >= 9,
            "the repo's own highlight.yaml loads (" + std::to_string(shipped.languages()) + " languages)");
        chk(shipped.find("cpp") != nullptr && shipped.find("c++") != nullptr, "a fence tag and its alias resolve");
        chk(shipped.find("cpp")->quotes.size() == 2, "an undeclared quote list is the pair every language shares");
        chk(shipped.find("json")->quotes.size() == 1, "json declares one delimiter");
        chk(shipped.find("html")->line_comment.empty(),
            "a declared-none line comment is none, not a prefix that matches every row");

        CodeCtx h;
        h.set = &shipped;
        h.lang = shipped.find("html");
        h.fence = true;
        const auto hspans = highlight_row("# not a comment in html", th, h);
        chk(!std::any_of(hspans.begin(), hspans.end(), [&](const Span& s) { return s.st.fg == th.code_cmt; }),
            "a row starting with # under html is not a comment");

        CodeCtx j;
        j.set = &shipped;
        j.lang = shipped.find("json");
        j.fence = true;
        const auto jspans = highlight_row("\"tone\": \"amber\"", th, j);
        chk(std::any_of(jspans.begin(), jspans.end(),
                        [&](const Span& s) { return s.text == "\"tone\"" && s.st.fg == th.code_ty; }),
            "a json key colours as a member name, not as a value");

        CodeCtx py;
        py.set = &shipped;
        py.lang = shipped.find("python");
        py.fence = true;
        const auto pyspans = highlight_row("def load(path):  # read it", th, py);
        std::string back;
        for (const auto& s : pyspans) back += s.text;
        chk(back == "def load(path):  # read it", "colouring rewrites nothing, even from a shipped file");
        chk(std::any_of(pyspans.begin(), pyspans.end(),
                        [&](const Span& s) { return s.text == "def" && s.st.fg == th.code_kw; }),
            "a keyword the file declares colours");
        chk(std::any_of(pyspans.begin(), pyspans.end(),
                        [&](const Span& s) { return s.st.fg == th.code_cmt; }),
            "the file's comment marker colours the tail");

        // A language with no file behind it keeps the shared facts and claims no keyword: the
        // shipped file is what makes a keyword a keyword, not this binary.
        CodeCtx g;
        g.set = &shipped;
        highlight_row("```nosuchlang", th, g);
        chk(g.lang == nullptr, "an unknown tag names no language");
        const auto gspans = highlight_row("widget = \"x\" # note", th, g);
        chk(std::any_of(gspans.begin(), gspans.end(), [&](const Span& s) { return s.st.fg == th.code_str; }),
            "under an unknown tag a string is still a string");
        chk(std::any_of(gspans.begin(), gspans.end(), [&](const Span& s) { return s.st.fg == th.code_cmt; }),
            "under an unknown tag a comment is still a comment");
        const auto w = std::find_if(gspans.begin(), gspans.end(), [&](const Span& s) { return s.text == "widget"; });
        chk(w != gspans.end() && w->st.fg == th.text, "under an unknown tag no word is claimed as a keyword");
    }

    {   // a tool result that is code colours like a fence of its language -- the operator's
        // complaint: a `sed` of a .py file printed as one grey wall while a thought's yaml fence lit up
        const std::string yaml =
            "languages:\n"
            "  python:\n    aliases: [py]\n    keywords: [def, return, class]\n    capital_is_type: false\n"
            "  json:\n    keys_are_types: true\n";
        struct Out { std::string ansi, kw, accent, muted; };
        auto card = [&](const std::string& tool, const std::string& args, const std::string& result) {
            Scene s{Theme{}};
            s.set_grammars(GrammarSet::from_yaml(yaml, "test"));
            // what a ```python fence makes of `def`, against this scene's own reconciled palette
            GrammarSet same = GrammarSet::from_yaml(yaml, "test");
            same.reconcile(s.theme());
            CodeCtx fx;
            fx.set = &same;
            highlight_row("```python", s.theme(), fx);
            Out o;
            for (const auto& sp : highlight_row("def f():", s.theme(), fx))
                if (sp.text == "def") o.kw = sgr(sp.st, true) + "def";
            o.accent = sgr(Style{s.theme().accent}, true);
            o.muted = sgr(Style{s.theme().muted}, true);
            s.turn_begin();
            s.tool_call("c1", tool, args);
            s.tool_result("c1", result);
            s.turn_end();
            o.ansi = s.drain_inline(100, true, true);
            return o;
        };
        const std::string py_out = R"({"exit":0,"stdout":"def f():\n    return 1\n"})";
        Out o = card("run_terminal", R"({"cmd":"cd ~/x && sed -n '1,5p' tools/run.py"})", py_out);
        chk(!o.kw.empty() && o.ansi.find(o.kw) != std::string::npos, "sed on a .py file: the rows colour as python");
        chk(o.ansi.find(o.muted + "exit 0 · ") != std::string::npos, "the harness's `exit 0 · ` stays muted in front of the code");
        o = card("run_terminal", R"({"cmd":"python3 tools/run.py"})", py_out);
        chk(o.ansi.find(o.kw) == std::string::npos, "running a .py file prints its output, not its source: no language");
        o = card("read_file", R"({"path":"src/x.py"})", R"({"content":"def g():\n    return 2\n"})");
        chk(o.ansi.find(o.kw) != std::string::npos, "read_file of a .py colours as python");
        o = card("run_terminal", R"({"cmd":"grep -rn def src"})",
                 R"({"exit":0,"stdout":"src/a.py:12:def h():\nsrc/b.txt:3:def is a word\n"})");
        auto count = [](const std::string& hay, const std::string& needle) {
            size_t n = 0;
            for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) ++n;
            return n;
        };
        chk(count(o.ansi, o.kw) == 1, "of two grep rows only the .py one colours `def` as python (the .txt one is prose)");
        chk(o.ansi.find(o.accent + "src/a.py") != std::string::npos && o.ansi.find(o.accent + "src/b.txt") != std::string::npos,
            "every grep row sets its path apart");
        o = card("run_terminal", R"({"cmd":"grep -n def tools/run.py"})", R"({"exit":0,"stdout":"12:def k():\n"})");
        chk(o.ansi.find(o.kw) != std::string::npos, "a grep on one file prints no path: its operand names the language");
        o = card("run_terminal", R"({"cmd":"cat tools/run.py"})", R"({"exit":1,"stdout":"def f():\n"})");
        chk(o.ansi.find(o.kw) == std::string::npos, "a failed call keeps its error colour");
        o = card("run_terminal", R"({"cmd":"make test"})", py_out);
        chk(o.ansi.find(o.kw) == std::string::npos, "a command that names no file says no language");
    }

    {   // a block opened on one row runs on: a docstring's prose is string, a /* comment's body is comment
        const GrammarSet set = GrammarSet::from_yaml(
            "languages:\n"
            "  python:\n    keywords: [def, return]\n    capital_is_type: false\n"
            "  c:\n    keywords: [return, int]\n    block_comment: {open: \"/*\", close: \"*/\"}\n", "test");
        Theme th;
        CodeCtx ctx;
        ctx.set = &set;
        highlight_row("```python", th, ctx);
        auto spans_of = [&](const std::string& row) { return highlight_row(row, th, ctx); };
        auto styled = [](const std::vector<Span>& sp, const std::string& text) -> const Span* {
            for (const auto& x : sp)
                if (x.text.find(text) != std::string::npos) return &x;
            return nullptr;
        };
        const auto open = spans_of("def f():");
        const Style kw = styled(open, "def")->st;
        const auto d1 = spans_of("    \"\"\"Summary, and you return here.");
        const auto d2 = spans_of("    it's prose: you return nothing here");
        const auto d3 = spans_of("    \"\"\"");
        const auto after = spans_of("    return 1");
        chk(styled(d1, "you return here") && styled(d1, "you return here")->st.fg == th.code_str,
            "a docstring's first row is string to its end");
        chk(d2.size() == 1 && d2[0].st.fg == th.code_str && !(d2[0].st == kw),
            "its next row is string too: no keyword in the prose, no apostrophe opening another string");
        chk(styled(d3, "\"\"\"") && styled(d3, "\"\"\"")->st.fg == th.code_str, "the closing quotes are string");
        chk(styled(after, "return") && styled(after, "return")->st == kw, "after the close the code is code again");
        highlight_row("```c", th, ctx);
        const auto c1 = spans_of("int x; /* a comment that");
        const auto c2 = spans_of("   return goes on */ int y;");
        chk(styled(c1, "/* a comment that")->st.fg == th.code_cmt, "a block comment opens");
        chk(styled(c2, "return goes on */") && styled(c2, "return goes on */")->st.fg == th.code_cmt,
            "its next row is comment up to the close");
        chk(styled(c2, "int") && styled(c2, "int")->st.fg != th.code_cmt, "and code after it");
        highlight_row("```python", th, ctx);
        spans_of("    \"\"\"left open");
        highlight_row("```", th, ctx);
        chk(ctx.carry.empty(), "a fence row ends a block the last one left open");
    }

    if (g_fail) std::printf("%d FAILED\n", g_fail);
    else std::printf("ui: all passed (best kernels: %s)\n", best.c_str());
    return g_fail ? 1 : 0;
}