// Port of tools/ansi_strip.py — ECMA-48 / ANSI escape stripping.
#include "dd/tools/ansi_strip.hpp"

#include <string>
#include <vector>
#include <cstdint>

namespace dd {
namespace tools {

// Decode UTF-8 to codepoints. A byte that does not start a well-formed sequence (RFC 3629: no overlongs, no
// surrogates, nothing past U+10FFFF) is emitted as its byte value and advances one byte (loose
// surrogateescape-style fallback).
static std::vector<uint32_t> decode_utf8(const std::string& s) {
    std::vector<uint32_t> cps;
    cps.reserve(s.size());
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char b0 = static_cast<unsigned char>(s[i]);
        if (b0 < 0x80) { cps.push_back(b0); ++i; continue; }
        size_t len = 0;
        if ((b0 & 0xE0) == 0xC0) len = 2;
        else if ((b0 & 0xF0) == 0xE0) len = 3;
        else if ((b0 & 0xF8) == 0xF0) len = 4;
        if (len >= 2 && i + len <= n) {
            bool ok = true;
            for (size_t k = 1; k < len; ++k) {
                unsigned char bk = static_cast<unsigned char>(s[i + k]);
                if ((bk & 0xC0) != 0x80) { ok = false; break; }
            }
            if (ok) {
                uint32_t cp = b0 & (0xFF >> (len + 1));
                for (size_t k = 1; k < len; ++k)
                    cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
                const uint32_t least = len == 2 ? 0x80u : len == 3 ? 0x800u : 0x10000u;
                if (cp >= least && !(cp >= 0xD800 && cp <= 0xDFFF) && cp <= 0x10FFFF) {
                    cps.push_back(cp);
                    i += len;
                    continue;
                }
            }
        }
        cps.push_back(b0);
        ++i;
    }
    return cps;
}

static void encode_utf8(uint32_t cp, std::string& out) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Leftmost ANSI/ECMA-48 escape match starting at cps[i]; returns exclusive end
// index, or -1 if no match begins at i. Mirrors the ordered alternation of
// _ANSI_ESCAPE_RE with greedy/lazy semantics.
static int match_ansi_at(const std::vector<uint32_t>& cps, size_t i) {
    const size_t n = cps.size();
    const uint32_t c = cps[i];

    // Group B: 8-bit CSI (0x9b) params/intermediates/final.
    if (c == 0x9B) {
        size_t j = i + 1;
        while (j < n && cps[j] >= 0x30 && cps[j] <= 0x3F) ++j;
        while (j < n && cps[j] >= 0x20 && cps[j] <= 0x2F) ++j;
        if (j < n && cps[j] >= 0x40 && cps[j] <= 0x7E) return static_cast<int>(j + 1);
        return -1;
    }

    // Group C: 8-bit OSC (0x9d) lazy to BEL or ST(0x9c).
    if (c == 0x9D) {
        for (size_t j = i + 1; j < n; ++j) {
            if (cps[j] == 0x07) return static_cast<int>(j + 1);
            if (cps[j] == 0x9C) return static_cast<int>(j + 1);
        }
        return -1;
    }

    // Group D: bare 8-bit C1 control (0x80-0x9f).
    if (c >= 0x80 && c <= 0x9F) return static_cast<int>(i + 1);

    if (c != 0x1B) return -1;  // ESC group
    if (i + 1 >= n) return -1;  // lone ESC matches nothing
    const uint32_t d = cps[i + 1];

    // A1: CSI  ESC '[' params(0x30-0x3f)* intermediates(0x20-0x2f)* final(0x40-0x7e)
    if (d == 0x5B) {  // '['
        size_t j = i + 2;
        while (j < n && cps[j] >= 0x30 && cps[j] <= 0x3F) ++j;
        while (j < n && cps[j] >= 0x20 && cps[j] <= 0x2F) ++j;
        if (j < n && cps[j] >= 0x40 && cps[j] <= 0x7E) return static_cast<int>(j + 1);
        // CSI failed; fall through to A5 (Fe) which matches ESC + '[' (0x5b in 0x30-0x7e).
    }

    // A2: OSC  ESC ']' lazy to BEL or ESC-backslash
    if (d == 0x5D) {  // ']'
        for (size_t j = i + 2; j < n; ++j) {
            if (cps[j] == 0x07) return static_cast<int>(j + 1);
            if (cps[j] == 0x1B && j + 1 < n && cps[j + 1] == 0x5C) return static_cast<int>(j + 2);
        }
        return -1;
    }

    // A3: DCS/SOS/PM/APC  ESC [PX^_] lazy to ESC-backslash
    if (d == 0x50 || d == 0x58 || d == 0x5E || d == 0x5F) {
        for (size_t j = i + 2; j < n; ++j) {
            if (cps[j] == 0x1B && j + 1 < n && cps[j + 1] == 0x5C) return static_cast<int>(j + 2);
        }
        return -1;
    }

    // A4: nF  ESC (0x20-0x2f)+ final(0x30-0x7e)
    if (d >= 0x20 && d <= 0x2F) {
        size_t j = i + 2;
        while (j < n && cps[j] >= 0x20 && cps[j] <= 0x2F) ++j;  // greedy + (already >=1)
        if (j < n && cps[j] >= 0x30 && cps[j] <= 0x7E) return static_cast<int>(j + 1);
        return -1;
    }

    // A5: Fp/Fe/Fs  ESC single byte 0x30-0x7e
    if (d >= 0x30 && d <= 0x7E) return static_cast<int>(i + 2);

    return -1;
}

static bool has_escape(const std::vector<uint32_t>& cps) {
    for (uint32_t c : cps)
        if (c == 0x1B || (c >= 0x80 && c <= 0x9F)) return true;
    return false;
}

static std::string strip_ansi_impl(const std::vector<uint32_t>& cps) {
    std::string out;
    const size_t n = cps.size();
    size_t i = 0;
    while (i < n) {
        int end = match_ansi_at(cps, i);
        if (end >= 0) { i = static_cast<size_t>(end); continue; }  // remove
        encode_utf8(cps[i], out);
        ++i;
    }
    return out;
}

std::string strip_ansi(const std::string& text) {
    if (text.empty()) return text;
    std::vector<uint32_t> cps = decode_utf8(text);
    if (!has_escape(cps)) return text;  // fast path
    return strip_ansi_impl(cps);
}

// HAS_CONTROL = [\x00-\x08 \x0b-\x1f \x7f-\x9f]
static bool has_control(const std::vector<uint32_t>& cps) {
    for (uint32_t c : cps) {
        if (c <= 0x08) return true;
        if (c >= 0x0B && c <= 0x1F) return true;
        if (c >= 0x7F && c <= 0x9F) return true;
    }
    return false;
}

// CONTROL_CHARS_RE = [\x00-\x08 \x0b \x0c \x0e-\x1f \x7f]
static bool is_control_char(uint32_t c) {
    if (c <= 0x08) return true;
    if (c == 0x0B || c == 0x0C) return true;
    if (c >= 0x0E && c <= 0x1F) return true;
    if (c == 0x7F) return true;
    return false;
}

std::string sanitize_display_text(const std::string& text) {
    if (text.empty()) return text;
    std::vector<uint32_t> cps = decode_utf8(text);
    if (!has_control(cps)) return text;  // fast path
    // strip_ansi (re-decodes internally; feed its own fast path)
    std::string stripped = strip_ansi(text);
    // normalize CRLF -> LF, then CR -> LF
    std::string norm;
    norm.reserve(stripped.size());
    for (size_t i = 0; i < stripped.size(); ++i) {
        if (stripped[i] == '\r') {
            if (i + 1 < stripped.size() && stripped[i + 1] == '\n') continue;  // CRLF -> LF (skip CR)
            norm.push_back('\n');
        } else {
            norm.push_back(stripped[i]);
        }
    }
    // remove bare control chars (codepoint-aware)
    std::vector<uint32_t> cps2 = decode_utf8(norm);
    std::string out;
    for (uint32_t c : cps2)
        if (!is_control_char(c)) encode_utf8(c, out);
    return out;
}

}  // namespace tools
}  // namespace dd
