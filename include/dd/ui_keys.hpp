#pragma once
// dd/ui_keys.hpp — the prompt's input side: raw terminal bytes -> key events, and the line editor.
//
// An escape that ends a read() is HELD: it is the start of a sequence whose rest has not arrived yet, or a real Esc.
// The loop decides by time: flush_esc() after ~25 ms of silence, drop_partial() for a half sequence after ~250 ms.

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "dd/ui.hpp"

namespace dd {
namespace ui {

enum class K { Char, Paste, Enter, Backspace, Delete, Left, Right, WordLeft, WordRight, Up, Down, Home, End,
               PgUp, PgDn, WheelUp, WheelDown, Tab, Esc, CtrlC, CtrlD, CtrlK, CtrlL, CtrlU, CtrlW };

struct Key {
    K k;
    std::u32string text;
    int x = -1, y = -1;   // a wheel's cell, 0-based; -1 when the terminal did not say
};

class KeyReader {
public:
    // Complete keys from what has arrived; a split sequence waits for the rest.
    std::vector<Key> feed(const char* p, size_t n) {
        buf_.append(p, n);
        std::vector<Key> out;
        auto key = [&](K k) { out.push_back(Key{k, {}}); };
        size_t i = 0;
        while (i < buf_.size()) {
            if (in_paste_) {
                const size_t e = buf_.find("\x1b[201~", i);
                if (e == std::string::npos) {
                    const size_t keep = std::min<size_t>(5, buf_.size() - i);   // maybe half a terminator
                    paste_.append(buf_, i, buf_.size() - i - keep);
                    i = buf_.size() - keep;
                    break;
                }
                paste_.append(buf_, i, e - i);
                std::string t;
                for (size_t k = 0; k < paste_.size(); ++k) {
                    if (paste_[k] == '\r') {
                        t.push_back('\n');
                        if (k + 1 < paste_.size() && paste_[k + 1] == '\n') ++k;
                    } else {
                        t.push_back(paste_[k]);
                    }
                }
                paste_.clear();
                out.push_back(Key{K::Paste, from_utf8(t)});
                in_paste_ = false;
                i = e + 6;
                continue;
            }
            const unsigned char c = static_cast<unsigned char>(buf_[i]);
            if (c == 0x1b) {
                if (i + 1 >= buf_.size()) break;   // held: the rest may be one read away
                const char n1 = buf_[i + 1];
                if (n1 == 0x1b) {   // Esc Esc: the first is a key of its own
                    key(K::Esc);
                    ++i;
                    continue;
                }
                if (n1 == '[') {
                    size_t j = i + 2;
                    while (j < buf_.size() && (static_cast<unsigned char>(buf_[j]) < 0x40 || static_cast<unsigned char>(buf_[j]) > 0x7E)) ++j;
                    if (j >= buf_.size()) break;
                    const std::string par = buf_.substr(i + 2, j - i - 2);
                    const char fin = buf_[j];
                    i = j + 1;
                    if (!par.empty() && par[0] == '<') {
                        const int b = std::atoi(par.c_str() + 1);
                        int cx = -1, cy = -1;   // "<b;x;y" -- SGR cells are 1-based
                        if (const size_t s1 = par.find(';'); s1 != std::string::npos) {
                            cx = std::atoi(par.c_str() + s1 + 1) - 1;
                            if (const size_t s2 = par.find(';', s1 + 1); s2 != std::string::npos)
                                cy = std::atoi(par.c_str() + s2 + 1) - 1;
                        }
                        if (fin == 'M' && b == 64) out.push_back(Key{K::WheelUp, {}, cx, cy});
                        if (fin == 'M' && b == 65) out.push_back(Key{K::WheelDown, {}, cx, cy});
                        continue;
                    }
                    const bool ctrl = par.find(";5") != std::string::npos;
                    switch (fin) {
                    case 'A': key(K::Up); break;
                    case 'B': key(K::Down); break;
                    case 'C': key(ctrl ? K::WordRight : K::Right); break;
                    case 'D': key(ctrl ? K::WordLeft : K::Left); break;
                    case 'H': key(K::Home); break;
                    case 'F': key(K::End); break;
                    case '~': {
                        const int code = std::atoi(par.c_str());
                        if (code == 1 || code == 7) key(K::Home);
                        else if (code == 4 || code == 8) key(K::End);
                        else if (code == 3) key(K::Delete);
                        else if (code == 5) key(K::PgUp);
                        else if (code == 6) key(K::PgDn);
                        else if (code == 200) in_paste_ = true;
                        break;
                    }
                    default: break;
                    }
                    continue;
                }
                if (n1 == 'O') {
                    if (i + 2 >= buf_.size()) break;
                    switch (buf_[i + 2]) {
                    case 'A': key(K::Up); break;
                    case 'B': key(K::Down); break;
                    case 'C': key(K::Right); break;
                    case 'D': key(K::Left); break;
                    case 'H': key(K::Home); break;
                    case 'F': key(K::End); break;
                    default: break;
                    }
                    i += 3;
                    continue;
                }
                if (n1 == 0x7f || n1 == 0x08) key(K::CtrlW);   // alt-backspace
                else if (n1 == 'b') key(K::WordLeft);
                else if (n1 == 'f') key(K::WordRight);
                i += 2;
                continue;
            }
            if (c == '\r' || c == '\n') { key(K::Enter); ++i; continue; }
            if (c == 0x7f || c == 0x08) { key(K::Backspace); ++i; continue; }
            if (c < 0x20) {
                switch (c) {
                case '\t': key(K::Tab); break;
                case 0x01: key(K::Home); break;
                case 0x02: key(K::Left); break;
                case 0x03: key(K::CtrlC); break;
                case 0x04: key(K::CtrlD); break;
                case 0x05: key(K::End); break;
                case 0x06: key(K::Right); break;
                case 0x0b: key(K::CtrlK); break;
                case 0x0c: key(K::CtrlL); break;
                case 0x0e: key(K::Down); break;
                case 0x10: key(K::Up); break;
                case 0x15: key(K::CtrlU); break;
                case 0x17: key(K::CtrlW); break;
                default: break;
                }
                ++i;
                continue;
            }
            const size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
            if (i + len > buf_.size()) break;
            out.push_back(Key{K::Char, from_utf8(buf_.substr(i, len))});
            i += len;
        }
        buf_.erase(0, i);
        return out;
    }

    // A lone Esc is held. No more bytes after the loop's patience: it was the key.
    bool esc_pending() const { return !in_paste_ && buf_.size() == 1 && buf_[0] == 0x1b; }
    std::vector<Key> flush_esc() {
        std::vector<Key> out;
        if (!esc_pending()) return out;
        buf_.clear();
        out.push_back(Key{K::Esc, {}});
        return out;
    }
    // A sequence that began and never ended (a stalled link, a stray Alt-[): neither Esc nor text.
    bool partial_pending() const { return !in_paste_ && buf_.size() > 1 && buf_[0] == 0x1b; }
    void drop_partial() {
        if (partial_pending()) buf_.clear();
    }

private:
    std::string buf_, paste_;
    bool in_paste_ = false;
};

// ---- the prompt line: UTF-8 aware editing + history ----
struct Editor {
    std::u32string buf;
    size_t cur = 0;
    std::vector<std::u32string> hist;
    size_t hpos = 0;
    std::u32string draft;

    void insert(const std::u32string& s) { buf.insert(cur, s); cur += s.size(); }
    void backspace() { if (cur) buf.erase(--cur, 1); }
    void del() { if (cur < buf.size()) buf.erase(cur, 1); }
    void word_left() {
        while (cur && buf[cur - 1] == U' ') --cur;
        while (cur && buf[cur - 1] != U' ') --cur;
    }
    void word_right() {
        while (cur < buf.size() && buf[cur] == U' ') ++cur;
        while (cur < buf.size() && buf[cur] != U' ') ++cur;
    }
    void kill_word() {
        const size_t e = cur;
        word_left();
        buf.erase(cur, e - cur);
    }
    void clear() {
        buf.clear();
        cur = 0;
        hpos = hist.size();
    }
    void recall(int dir) {
        if (hist.empty()) return;
        if (hpos == hist.size()) draft = buf;
        if (dir < 0 && hpos > 0) --hpos;
        else if (dir > 0 && hpos < hist.size()) ++hpos;
        else return;
        buf = hpos == hist.size() ? draft : hist[hpos];
        cur = buf.size();
    }
    std::u32string take() {
        std::u32string s = buf;
        if (!s.empty() && (hist.empty() || hist.back() != s)) hist.push_back(s);
        clear();
        return s;
    }
};

}  // namespace ui
}  // namespace dd
