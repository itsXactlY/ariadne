// dd::ui — kernels vs scalar on every SIMD level the CPU has, the lock-free
// queue under contention, text metrics, frame diffing, layout geometry.
#include "dd/ui.hpp"
#include "dd/ui_simd.hpp"

#include <omp.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace dd::ui;

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

    // ---- MPSC queue: 8 producers, consumer racing them; FIFO per producer ----
    {
        MpscQueue<std::pair<int, int>> q;
        constexpr int P = 8, N = 100000;
        std::atomic<int> done{0};
        std::vector<std::thread> ts;
        for (int p = 0; p < P; ++p)
            ts.emplace_back([&, p] {
                for (int i = 0; i < N; ++i) q.push({p, i});
                ++done;
            });
        std::vector<int> next(P, 0);
        long got = 0;
        bool order = true;
        std::pair<int, int> v;
        while (got < static_cast<long>(P) * N) {
            if (q.pop(v)) {
                order = order && v.second == next[static_cast<size_t>(v.first)];
                ++next[static_cast<size_t>(v.first)];
                ++got;
            }
        }
        for (auto& t : ts) t.join();
        chk(order && got == static_cast<long>(P) * N && !q.pop(v), "mpsc fifo + count");
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
        chk(s.row_text(0).rfind("╭─┤ ⟁ DAEDALUS ├┤ c++ ├", 0) == 0, "transcript title");
        chk(s.row_text(0).find("session") != std::string::npos, "sidebar at 120 cols");
        chk(s.row_text(26).rfind("╭─┤ you ├", 0) == 0 && s.row_text(28).rfind("╰", 0) == 0, "prompt box rows");
        chk(s.row_text(29).find("ready") != std::string::npos, "status row");
        chk(s.present(120, 30, true).empty(), "idle frame writes nothing");
        s.present(90, 30, true);
        chk(s.row_text(0).find("session") == std::string::npos, "no sidebar at 90 cols");
        s.present(50, 10, true);
        chk(s.row_text(5).find("needs") != std::string::npos, "too small");
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
        chk(out.size() == 12, "tool cards are 6 rows each");
        const std::string tail = "─ ✓ done ┐";
        chk(out[0].rfind("┌ ▸ run_terminal ─", 0) == 0 && out[0].size() > tail.size() &&
            out[0].compare(out[0].size() - tail.size(), tail.size(), tail) == 0, "tool card header");
        s.tool_call("c3", "list_dir", "{}");
        chk(s.drain_inline(80, false, false).empty(), "unresolved card waits");
        s.turn_end();
        const std::string rest = s.drain_inline(80, false, false);
        chk(rest.find("cancelled") != std::string::npos && rest.find("turn 1") != std::string::npos, "turn end resolves");
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
        omp_set_num_threads(1);
        const std::string a = s1->present(240, 80, true);
        omp_set_num_threads(8);
        const std::string b = s8->present(240, 80, true);
        bool rows = a == b;
        for (int y = 0; y < 80; ++y) rows = rows && s1->row_text(y) == s8->row_text(y);
        chk(rows, "parallel present matches serial");
    }

    if (g_fail) std::printf("%d FAILED\n", g_fail);
    else std::printf("ui: all passed (best kernels: %s)\n", best.c_str());
    return g_fail ? 1 : 0;
}
