// dd::ui — kernels vs scalar on every SIMD level the CPU has, the slot ring under
// contention, text metrics, frame diffing, layout geometry.
#include "dd/ui.hpp"
#include "dd/ui_simd.hpp"

#ifdef _OPENMP
#include <omp.h>   // the parallel-vs-serial check needs the runtime; the TSan build has none
#endif

#include <atomic>
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
        auto with_plan = [&](Verbosity v) {
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
            s.present(120, 30, true);
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
            chk(out[0].find("◈ thought") != std::string::npos && out[0].find("40 tok") != std::string::npos, "reason header");
            chk(out[R].find("step 39") != std::string::npos && out[1].find("step 34") != std::string::npos, "tail = newest lines");
            chk(out[R + 2].find("↑ 34 lines") != std::string::npos, "hidden-lines marker under the border");
            chk(out[R + 3].find("ARIADNE") != std::string::npos && out[R + 3].find("1.2s") != std::string::npos &&
                    out[R + 4].find("The answer.") != std::string::npos, "streamed answer card");
        }
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

    if (g_fail) std::printf("%d FAILED\n", g_fail);
    else std::printf("ui: all passed (best kernels: %s)\n", best.c_str());
    return g_fail ? 1 : 0;
}
