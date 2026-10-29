// tests/ui_keys_test.cpp — the prompt's input side: a sequence split anywhere in the byte stream gives the keys it gives whole,
// a lone Esc is held until the loop says it was the key, and the line editor edits.
#include "dd/ui_keys.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace dd::ui;

static int g_fail = 0, g_pass = 0;
static void chk(bool ok, const std::string& what) {
    if (ok) ++g_pass;
    else { ++g_fail; std::printf("FAIL %s\n", what.c_str()); }
}

static std::string name(const Key& k) {
    switch (k.k) {
    case K::Char: return "Char:" + to_utf8(k.text);
    case K::Paste: return "Paste:" + to_utf8(k.text);
    case K::Enter: return "Enter";
    case K::Backspace: return "Backspace";
    case K::Delete: return "Delete";
    case K::Left: return "Left";
    case K::Right: return "Right";
    case K::WordLeft: return "WordLeft";
    case K::WordRight: return "WordRight";
    case K::Up: return "Up";
    case K::Down: return "Down";
    case K::Home: return "Home";
    case K::End: return "End";
    case K::PgUp: return "PgUp";
    case K::PgDn: return "PgDn";
    case K::WheelUp: return "WheelUp";
    case K::WheelDown: return "WheelDown";
    case K::Tab: return "Tab";
    case K::Esc: return "Esc";
    case K::CtrlC: return "CtrlC";
    case K::CtrlD: return "CtrlD";
    case K::CtrlK: return "CtrlK";
    case K::CtrlL: return "CtrlL";
    case K::CtrlU: return "CtrlU";
    case K::CtrlW: return "CtrlW";
    }
    return "?";
}

// the keys the chunks give, a held Esc flushed at the end as the loop does after its silence
static std::string run(const std::vector<std::string>& chunks) {
    KeyReader r;
    std::string out;
    auto add = [&](const std::vector<Key>& ks) {
        for (const auto& k : ks) out += name(k) + " ";
    };
    for (const auto& c : chunks) add(r.feed(c.data(), c.size()));
    if (r.esc_pending()) add(r.flush_esc());
    return out;
}

static std::string show(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        char b[8];
        if (c < 0x20 || c >= 0x7f) std::snprintf(b, sizeof b, "\\x%02x", c);
        else std::snprintf(b, sizeof b, "%c", c);
        o += b;
    }
    return o;
}

// every way to cut `s` in two, and a few in three
static void all_splits(const std::string& s, const std::string& want) {
    chk(run({s}) == want, "whole " + show(s) + " -> " + run({s}) + " (want " + want + ")");
    for (size_t p = 1; p < s.size(); ++p) {
        const std::string got = run({s.substr(0, p), s.substr(p)});
        chk(got == want, "split " + show(s) + " at " + std::to_string(p) + " -> " + got);
    }
    for (size_t a = 1; a + 1 < s.size(); ++a)
        for (size_t b = a + 1; b < s.size(); ++b) {
            const std::string got = run({s.substr(0, a), s.substr(a, b - a), s.substr(b)});
            chk(got == want, "split3 " + show(s) + " at " + std::to_string(a) + "," + std::to_string(b) + " -> " + got);
        }
}

int main() {
    // ---- every split point of the sequences a terminal sends ----
    all_splits("\x1b[A", "Up ");
    all_splits("\x1b[B", "Down ");
    all_splits("\x1b[1;5C", "WordRight ");
    all_splits("\x1b[1;5D", "WordLeft ");
    all_splits("\x1b[3~", "Delete ");
    all_splits("\x1b[5~\x1b[6~", "PgUp PgDn ");
    all_splits("\x1b[<64;10;5M", "WheelUp ");
    all_splits("\x1b[<65;120;45M", "WheelDown ");
    all_splits("\x1b[<0;10;5M\x1b[<0;10;5m", "");   // a click and its release say nothing
    all_splits("\x1bOA", "Up ");
    all_splits("\x1bOH", "Home ");
    all_splits("\x1b[200~hello\r\nworld\x1b[201~", "Paste:hello\nworld ");
    all_splits("\x1b[200~a\x1b[Ab\x1b[201~x", "Paste:a\x1b[Ab Char:x ");   // keys inside a paste stay text
    all_splits("ab\x1b[Bcd", "Char:a Char:b Down Char:c Char:d ");
    all_splits("\x1b" "b\x1b" "f", "WordLeft WordRight ");
    all_splits("\xc3\xa9\xe6\x97\xa5\xf0\x9f\x98\x80", "Char:\xc3\xa9 Char:\xe6\x97\xa5 Char:\xf0\x9f\x98\x80 ");   // é 日 😀 cut mid-character

    // ---- a lone Esc is held, never emitted by feed() ----
    {
        KeyReader r;
        chk(r.feed("\x1b", 1).empty(), "lone Esc: feed emits nothing");
        chk(r.esc_pending(), "lone Esc: pending");
        chk(!r.partial_pending(), "lone Esc: not a partial sequence");
        const auto ks = r.flush_esc();
        chk(ks.size() == 1 && ks[0].k == K::Esc, "lone Esc: flush gives Esc");
        chk(!r.esc_pending(), "lone Esc: nothing held after the flush");
        chk(r.feed("a", 1).size() == 1, "after the flush the next key is its own");
    }
    // the reproduction from the audit: an arrow key split right after its ESC
    chk(run({"\x1b", "[A"}) == "Up ", "ESC | [A is Up, not Esc and stray text");
    chk(run({"\x1b", "[<64;10;5M"}) == "WheelUp ", "ESC | wheel is one WheelUp, no Esc, no stray text");
    {   // a wheel carries its cell (SGR is 1-based; the key is 0-based), so the sidebar can claim it
        KeyReader r;
        const std::string w = "\x1b[<65;120;45M";
        const auto ks = r.feed(w.data(), w.size());
        chk(ks.size() == 1 && ks[0].k == K::WheelDown && ks[0].x == 119 && ks[0].y == 44, "a wheel carries its 0-based cell");
        const std::string bare = "\x1b[<64M";
        const auto k2 = r.feed(bare.data(), bare.size());
        chk(k2.size() == 1 && k2[0].k == K::WheelUp && k2[0].x == -1 && k2[0].y == -1, "a wheel without a cell says -1, not 0");
    }
    chk(run({"\x1b"}) == "Esc ", "ESC with silence is Esc");
    chk(run({"a\x1b"}) == "Char:a Esc ", "text then a lone ESC");
    // Esc Esc: the first is a key, the second is held
    {
        KeyReader r;
        const auto ks = r.feed("\x1b\x1b", 2);
        chk(ks.size() == 1 && ks[0].k == K::Esc && r.esc_pending(), "Esc Esc: one key out, one held");
        chk(run({"\x1b\x1b"}) == "Esc Esc ", "Esc Esc is two Esc");
        chk(run({"\x1b\x1b[A"}) == "Esc Up ", "Esc then Up");
    }
    // a half sequence that never finishes is dropped without a key or text
    {
        KeyReader r;
        chk(r.feed("\x1b[<6", 4).empty(), "partial: nothing yet");
        chk(!r.esc_pending() && r.partial_pending(), "partial: held as a partial sequence");
        r.drop_partial();
        chk(!r.partial_pending() && !r.esc_pending(), "partial: dropped");
        const auto ks = r.feed("a", 1);
        chk(ks.size() == 1 && ks[0].k == K::Char && to_utf8(ks[0].text) == "a", "partial: the next key is clean");
        chk(r.flush_esc().empty(), "flush_esc with nothing held gives nothing");
    }
    // a lone ESC is not pending inside a paste
    {
        KeyReader r;
        r.feed("\x1b[200~abc", 8);
        chk(!r.esc_pending() && !r.partial_pending(), "inside a paste nothing is an Esc");
    }

    // ---- a paste cut at 4096-byte read() boundaries, anywhere, including inside both markers ----
    {
        std::string body;
        for (int i = 0; i < 9000; ++i) body += (i % 61 == 60) ? "\r\n" : std::string(1, static_cast<char>('a' + i % 26));
        std::string want_text;
        for (size_t i = 0; i < body.size(); ++i) {
            if (body[i] == '\r') {
                want_text += '\n';
                if (i + 1 < body.size() && body[i + 1] == '\n') ++i;
            } else {
                want_text += body[i];
            }
        }
        const std::string stream = "\x1b[200~" + body + "\x1b[201~" + "z";
        const std::string want = "Paste:" + want_text + " Char:z ";
        for (size_t lead = 0; lead < 12; ++lead) {   // moves the markers across the boundary
            const std::string s = std::string(lead, 'q') + stream;
            std::string w;
            for (size_t i = 0; i < lead; ++i) w += "Char:q ";
            w += want;
            for (size_t chunk : {1u, 2u, 3u, 5u, 6u, 7u, 4096u}) {
                std::vector<std::string> parts;
                for (size_t at = 0; at < s.size(); at += chunk) parts.push_back(s.substr(at, chunk));
                const std::string got = run(parts);
                chk(got == w, "paste lead " + std::to_string(lead) + " chunk " + std::to_string(chunk) + (got == w ? "" : " differs"));
            }
        }
    }

    // ---- plain keys ----
    chk(run({"\r"}) == "Enter ", "CR is Enter");
    chk(run({"\x7f\x08"}) == "Backspace Backspace ", "DEL and BS are Backspace");
    chk(run({"\x03\x04\x0b\x0c\x15\x17\t"}) == "CtrlC CtrlD CtrlK CtrlL CtrlU CtrlW Tab ", "control keys");

    // ---- the line editor ----
    {
        Editor e;
        e.insert(from_utf8("hello wörld"));
        chk(to_utf8(e.buf) == "hello wörld" && e.cur == 11, "editor insert");
        e.kill_word();
        chk(to_utf8(e.buf) == "hello " && e.cur == 6, "kill_word takes the last word");
        e.word_left();
        chk(e.cur == 0, "word_left to the start");
        e.word_right();
        chk(e.cur == 5, "word_right to the word's end");
        e.backspace();
        chk(to_utf8(e.buf) == "hell " && e.cur == 4, "backspace");
        e.del();
        chk(to_utf8(e.buf) == "hell" && e.cur == 4, "del at the end keeps the text");
        const auto first = e.take();
        chk(to_utf8(first) == "hell" && e.buf.empty() && e.cur == 0, "take returns the line and clears");
        e.insert(from_utf8("hell"));
        e.take();
        chk(e.hist.size() == 1, "the same line twice is one history entry");
        e.insert(from_utf8("second"));
        e.take();
        e.insert(from_utf8("draft"));
        e.recall(-1);
        chk(to_utf8(e.buf) == "second", "recall up: the newest");
        e.recall(-1);
        chk(to_utf8(e.buf) == "hell", "recall up again: the older");
        e.recall(-1);
        chk(to_utf8(e.buf) == "hell", "recall at the oldest stays");
        e.recall(+1);
        e.recall(+1);
        chk(to_utf8(e.buf) == "draft", "recall down returns to the draft");
    }

    std::printf("%d checks, %d failed\n", g_pass + g_fail, g_fail);
    if (g_fail == 0) std::printf("all ui keys tests passed\n");
    return g_fail == 0 ? 0 : 1;
}
