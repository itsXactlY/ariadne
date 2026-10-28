// src/ui.cpp — impl of dd/ui.hpp: cell grid, minimal-diff truecolor blit,
// skin theme, transcript cards, fixed panel layout.
#include "dd/ui.hpp"
#include "dd/core/config.hpp"
#include "dd/core/json.hpp"
#include "dd/tools/ansi_strip.hpp"
#include "dd/ui_simd.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace dd {
namespace ui {

namespace {

constexpr char32_t kSentinel = 0x110000;   // never a glyph: forces a repaint
// Below these a parallel region costs more than it saves (and wakes threads
// on a box whose cores belong to the model).
constexpr size_t kParCells = 1u << 15;
constexpr int kParDirty = 4096, kParBlocks = 8;

int par_threads() {
#ifdef _OPENMP
    return std::min(8, omp_get_max_threads());
#else
    return 1;
#endif
}
constexpr char32_t kSpin[] = {U'⠋', U'⠙', U'⠹', U'⠸', U'⠼', U'⠴', U'⠦', U'⠧', U'⠇', U'⠏'};

char32_t decode(const std::string& s, size_t& i) {
    const unsigned char b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80) { ++i; return b0; }
    const int len = (b0 & 0xE0) == 0xC0 ? 2 : (b0 & 0xF0) == 0xE0 ? 3 : (b0 & 0xF8) == 0xF0 ? 4 : 0;
    if (len == 0 || i + static_cast<size_t>(len) > s.size()) { ++i; return 0xFFFD; }
    char32_t v = b0 & (0x7Fu >> len);
    for (int k = 1; k < len; ++k) {
        const unsigned char b = static_cast<unsigned char>(s[i + static_cast<size_t>(k)]);
        if ((b & 0xC0) != 0x80) { ++i; return 0xFFFD; }
        v = (v << 6) | (b & 0x3Fu);
    }
    // RFC 3629: no overlongs, no surrogates, nothing past U+10FFFF (U+110000 is kSentinel: it must never be a glyph)
    const char32_t least = len == 2 ? 0x80 : len == 3 ? 0x800 : 0x10000;
    if (v < least || (v >= 0xD800 && v <= 0xDFFF) || v > 0x10FFFF) { ++i; return 0xFFFD; }
    i += static_cast<size_t>(len);
    return v;
}

void encode(std::string& o, char32_t c) {
    if (c < 0x80) {
        o.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
        o.push_back(static_cast<char>(0xC0 | (c >> 6)));
        o.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        o.push_back(static_cast<char>(0xE0 | (c >> 12)));
        o.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        o.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
        o.push_back(static_cast<char>(0xF0 | (c >> 18)));
        o.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
        o.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        o.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}

// Escapes and control bytes out, tabs to spaces.
std::string clean(const std::string& s) {
    std::string t = dd::tools::sanitize_display_text(s);
    std::string o;
    o.reserve(t.size());
    for (char c : t) {
        if (c == '\t') o += "    ";
        else o.push_back(c);
    }
    return o;
}

// Longest prefix of s that fits in w columns.
std::string cut(const std::string& s, int w) {
    int used = 0;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = i;
        const int cw = cp_width(decode(s, j));
        if (used + cw > w) break;
        used += cw;
        i = j;
    }
    return s.substr(0, i);
}

std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(' ');
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(' ') - b + 1);
}

// A streaming card keeps at most kLiveMax bytes; past it the oldest go (to kLiveKeep, so the cut is rare), never mid-character.
constexpr size_t kLiveMax = 512u << 10, kLiveKeep = 448u << 10;

void cap_live(std::string& t) {
    if (t.size() <= kLiveMax) return;
    size_t cut = t.size() - kLiveKeep;
    while (cut < t.size() && (static_cast<unsigned char>(t[cut]) & 0xC0) == 0x80) ++cut;
    t.erase(0, cut);
}

std::string hhmmss() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char b[16];
    std::strftime(b, sizeof b, "%H:%M:%S", &tm);
    return b;
}

std::string fmt_ms(double ms) {
    char b[32];
    if (ms < 1) return "<1ms";
    if (ms < 10) std::snprintf(b, sizeof b, "%.1fms", ms);
    else if (ms < 1000) std::snprintf(b, sizeof b, "%.0fms", ms);
    else if (ms < 60000) std::snprintf(b, sizeof b, "%.1fs", ms / 1000);
    else std::snprintf(b, sizeof b, "%dm%02ds", static_cast<int>(ms / 60000), static_cast<int>(std::fmod(ms / 1000, 60)));
    return b;
}

std::string fmt_tok(long n) {
    if (n < 1000) return std::to_string(n);
    char b[32];
    std::snprintf(b, sizeof b, "%.1fk", static_cast<double>(n) / 1000);
    return b;
}

// Tokens in one streamed delta. A delta is whatever bytes the endpoint handed over in one SSE
// chunk, so counting deltas is not counting tokens: a server that batches five tokens into one
// chunk would show 1. The estimate is the usual ~4 characters per token, charged per word-run so
// a punctuation-heavy run is not billed a token per mark. An estimate, never shown as a fact:
// `≈` while the call runs, and the endpoint's own number replaces it the moment the call ends.
static long approx_tokens(const std::string& s) {
    long t = 0;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\t' || s[i] == '\r')) ++i;
        size_t j = i;
        while (j < s.size() && s[j] != ' ' && s[j] != '\n' && s[j] != '\t' && s[j] != '\r') ++j;
        if (j > i) t += static_cast<long>((j - i + 3) / 4);
        i = j;
    }
    return t;
}

std::string group(long n) {   // 1009800 -> "1,009,800"; integers, no locale involved
    const std::string d = std::to_string(n < 0 ? 0 : n);
    std::string o;
    for (size_t i = 0; i < d.size(); ++i) {
        if (i && (d.size() - i) % 3 == 0) o += ',';
        o += d[i];
    }
    return o;
}

std::string fmt_bytes(long n) {
    char b[32];
    if (n < 1024) return std::to_string(n) + " B";
    if (n < (1 << 20)) std::snprintf(b, sizeof b, "%.1f KB", static_cast<double>(n) / 1024.0);
    else std::snprintf(b, sizeof b, "%.1f MB", static_cast<double>(n) / (1024.0 * 1024.0));
    return b;
}

std::string fmt1(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", v);
    return b;
}

int to256(unsigned r, unsigned g, unsigned b) {
    auto q = [](unsigned v) { return v < 48 ? 0 : v < 115 ? 1 : static_cast<int>((v - 35) / 40); };
    return 16 + 36 * q(r) + 6 * q(g) + q(b);
}

uint32_t tri(uint32_t a, uint32_t b, uint32_t c, double t) {
    return t < 0.5 ? mix(a, b, t * 2) : mix(b, c, (t - 0.5) * 2);
}

// `key="v"  n=3` from a JSON object; anything else as given.
std::string flat_args(const std::string& raw, int val_w) {
    bool repaired = false;
    const core::Json j = core::json_parse_args(raw, repaired);
    if (j.type != core::Json::Obj) return raw;  // not an object at all: show it as given
    std::string o;
    for (const auto& [k, v] : j.obj) {
        if (!o.empty()) o += "  ";
        o += k + "=";
        if (v && v->type == core::Json::Str) o += "\"" + clip(v->str, val_w) + "\"";
        else if (v) o += clip(core::json_dump(*v), val_w);
    }
    if (o.empty()) o = "{}";
    if (repaired) o += "  …cut";  // the model stopped mid-value: no half-values shown
    return o;
}

// What a result actually carried. The counts come from the reply's own accounting
// (tools/shell.cpp, tools/fs_tools.cpp), so a cut is reported by the tool that made
// it. Inferring one from how much the card happened to render would report the
// card's verbosity as the tool's cap -- the old claim in docs/PARITY.md.
struct ResultFacts {
    bool err = false;
    std::string body;
    long produced = 0, kept = 0, dropped = 0;   // bytes: command produced / model got / cut
    long lines_total = 0, lines_shown = 0, lines_dropped = 0, lines_cut = 0;
    long resume = 0;                            // fs next_offset: the line a continuation starts at
    bool scan_capped = false;                   // total_lines is a floor, not a fact
};

ResultFacts result_facts(const std::string& raw) {
    ResultFacts f;
    std::string s = raw;
    bool err = false;
    try {
        const core::Json j = core::json_parse(raw);
        if (j.type == core::Json::Obj) {
            const auto num = [&](const char* k) -> long {
                const core::Json* v = core::json_get(j, k);
                return v && v->type == core::Json::Num ? static_cast<long>(v->num) : 0;
            };
            const core::Json* e = core::json_get(j, "error");
            const core::Json* out = core::json_get(j, "stdout");
            const core::Json* content = core::json_get(j, "content");
            const core::Json* entries = core::json_get(j, "entries");
            const core::Json* is_err = core::json_get(j, "isError");
            if (e && e->type == core::Json::Str) {
                err = true;
                s = "error: " + e->str;
            } else if (out && out->type == core::Json::Str) {
                const core::Json* ex = core::json_get(j, "exit");
                s = "exit " + std::to_string(ex ? static_cast<long>(ex->num) : 0) + " · " + out->str;
                err = ex && ex->num != 0;
                // The budget's own split, not stdout's length: the truncation marker is
                // appended to stdout, so stdout.size() is kept + marker and would not
                // reconcile against produced - dropped.
                f.produced = num("output_bytes");
                f.dropped = num("bytes_dropped");
                f.kept = f.produced > 0 ? f.produced - f.dropped : static_cast<long>(out->str.size());
            } else if (content && content->type == core::Json::Str) {
                s = content->str;
                f.kept = static_cast<long>(content->str.size());
                f.lines_total = num("total_lines");
                f.lines_shown = num("lines");
                f.lines_dropped = num("lines_dropped");
                f.lines_cut = num("lines_cut");
                f.resume = num("next_offset");
                if (const core::Json* c = core::json_get(j, "total_lines_capped"); c && c->type == core::Json::Bool)
                    f.scan_capped = c->b;
            } else if (content && content->type == core::Json::Arr) {
                s.clear();
                for (const auto& part : content->arr)
                    if (part)
                        if (const core::Json* t = core::json_get(*part, "text"); t && t->type == core::Json::Str)
                            s += (s.empty() ? "" : "\n") + t->str;
                err = is_err && is_err->type == core::Json::Bool && is_err->b;
            } else if (entries && entries->type == core::Json::Arr) {
                s = std::to_string(entries->arr.size()) + " entries:";
                for (const auto& p : entries->arr)
                    if (p && p->type == core::Json::Str) {
                        const size_t sl = p->str.find_last_of('/');
                        s += " " + (sl == std::string::npos ? p->str : p->str.substr(sl + 1));
                    }
            } else {
                s = core::json_dump(j);
            }
        }
    } catch (...) {
    }
    std::string o;   // blank lines carry nothing in two rows
    size_t i = 0;
    while (i <= s.size()) {
        size_t nl = s.find('\n', i);
        if (nl == std::string::npos) nl = s.size();
        std::string line = s.substr(i, nl - i);
        if (line.find_first_not_of(" \t\r") != std::string::npos) o += (o.empty() ? "" : "\n") + line;
        i = nl + 1;
    }
    f.err = err;
    f.body = o;
    return f;
}

// The part of a tool result worth a glance; .first = it is an error.
std::pair<bool, std::string> preview(const std::string& raw) {
    const ResultFacts f = result_facts(raw);
    return {f.err, f.body};
}

bool failed_result(const std::string& raw) {
    return preview(raw).first;
}

// The header's truth about a result: what the tool cut, riding the status so even a
// folded card says it was cut. Forms run from most descriptive to tightest and the
// first one that fits the budget wins; empty means nothing fits, so the header keeps
// its timing and the accounting rows carry the number. A clipped "✓ done · …" is not
// a degraded datum -- it is a lie with an ellipsis.
std::string loss_datum(const ResultFacts& f, Verbosity v, int budget) {
    std::vector<std::string> forms;
    if (f.dropped > 0) {
        if (v == Verbosity::Verbose) forms.push_back(group(f.dropped) + " bytes dropped");
        forms.push_back(fmt_bytes(f.dropped) + " dropped");
        forms.push_back(fmt_bytes(f.dropped) + " cut");
    } else if (f.lines_dropped > 0) {
        forms.push_back(group(f.lines_dropped) + " lines dropped");
        forms.push_back(group(f.lines_dropped) + " lines cut");
        forms.push_back(group(f.lines_dropped) + " cut");
    } else if (f.lines_cut > 0) {
        forms.push_back(group(f.lines_cut) + " lines byte-cut");
        forms.push_back(group(f.lines_cut) + " cut");
    }
    for (const auto& form : forms)
        if (text_width(form) <= budget) return form;
    return {};
}

// The full split, verbose only: what reached the model, what the tool produced, and
// where a continuation starts. An error result gets nothing -- an error card already
// says what failed, and its bytes are not a truncated payload.
std::string result_account(const ResultFacts& f) {
    if (f.err) return {};
    std::string s;
    if (f.produced > 0)
        s = group(f.kept) + " of " + group(f.produced) + " bytes reached the model · " + group(f.dropped) + " dropped";
    else if (!f.body.empty())
        s = group(f.kept) + " bytes · whole result shown";
    else
        s = "empty result";
    if (f.lines_total > 0) {
        if (!s.empty()) s += " · ";
        s += group(f.lines_shown) + " of " + group(f.lines_total) + " lines";
        if (f.lines_dropped > 0)
            s += " · " + group(f.lines_dropped) + " dropped · continue at offset " +
                 group(f.resume > 0 ? f.resume : f.lines_shown + 1);
        if (f.lines_cut > 0) s += " · " + group(f.lines_cut) + " lines byte-cut";
        if (f.scan_capped) s += " · line total is a floor";
    }
    return s;
}

// The three-row ARIADNE mark, one column of space between letters.
const char* const kLogo[3] = {
    "╔═╗ ╔═╗ ╔═╗ ╔═╗ ╔╦╗ ╦ ╦ ╔═╗",
    "╠═╣ ╠═╝ ║║  ╠═╣  ║║ ║╱║ ║╣ ",
    "╩ ╩ ║ ╩ ╚═╝ ╩ ╩ ═╩╝ ╩ ╩ ╚═╝",
};

}  // namespace

// ---------------- text ----------------

uint32_t mix(uint32_t a, uint32_t b, double t) {
    if (a == kNone) return b;
    if (b == kNone) return a;
    t = std::clamp(t, 0.0, 1.0);
    auto ch = [&](int sh) {
        const double x = static_cast<double>((a >> sh) & 0xFF), y = static_cast<double>((b >> sh) & 0xFF);
        return static_cast<uint32_t>(std::lround(x + (y - x) * t)) << sh;
    };
    return rgb(ch(16) | ch(8) | ch(0));
}

int cp_width(char32_t c) {
    if (c < 0x20 || (c >= 0x7F && c < 0xA0)) return 0;
    if ((c >= 0x300 && c <= 0x36F) || (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x1DC0 && c <= 0x1DFF) ||
        (c >= 0x200B && c <= 0x200F) || (c >= 0x20D0 && c <= 0x20FF) || (c >= 0xFE00 && c <= 0xFE0F) ||
        c == 0xFEFF || (c >= 0xE0100 && c <= 0xE01EF))
        return 0;
    if ((c >= 0x1100 && c <= 0x115F) || c == 0x2329 || c == 0x232A || (c >= 0x2E80 && c <= 0x303E) ||
        (c >= 0x3041 && c <= 0x33FF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) ||
        (c >= 0xA000 && c <= 0xA4CF) || (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) ||
        (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6) ||
        (c >= 0x1F300 && c <= 0x1F64F) || (c >= 0x1F680 && c <= 0x1F6FF) || (c >= 0x1F900 && c <= 0x1F9FF) ||
        (c >= 0x1FA70 && c <= 0x1FAFF) || (c >= 0x20000 && c <= 0x3FFFD))
        return 2;
    return 1;
}

int text_width(const std::string& s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size();) {
        if (const size_t run = simd::ascii_run(s.data() + i, s.size() - i)) {
            n += simd::ascii_printable(s.data() + i, run);
            i += run;
            continue;
        }
        n += static_cast<size_t>(cp_width(decode(s, i)));
    }
    return static_cast<int>(n);
}

std::u32string from_utf8(const std::string& s) {
    std::u32string o(s.size(), U'\0');   // never more codepoints than bytes
    size_t n = 0;
    for (size_t i = 0; i < s.size();) {
        if (const size_t run = simd::ascii_run(s.data() + i, s.size() - i)) {
            simd::widen_ascii(s.data() + i, run, o.data() + n);
            i += run;
            n += run;
            continue;
        }
        o[n++] = decode(s, i);
    }
    o.resize(n);
    return o;
}

std::string to_utf8(const std::u32string& s) {
    std::string o;
    o.reserve(s.size());
    for (char32_t c : s) encode(o, c);
    return o;
}

// wrap, plus how many rows the cap did not show. One pass: wrap to full width-corrected
// rows, then take the head and mark the cut the same way wrap() does. A cap that reports
// nothing is a cap that reads as "that is the whole result".
std::vector<std::string> wrap_rows(const std::string& s, int width, int max_rows, long& hidden) {
    auto lines = wrap(s, width, 0);
    hidden = 0;
    if (max_rows > 0 && static_cast<int>(lines.size()) > max_rows) {
        hidden = static_cast<long>(lines.size()) - max_rows;
        lines.resize(static_cast<size_t>(max_rows));
        lines.back() = cut(lines.back(), std::max(1, width - 1)) + "…";
    }
    return lines;
}

std::vector<std::string> wrap(const std::string& s, int width, int max_rows) {
    width = std::max(1, width);
    std::vector<std::string> out;
    std::u32string line;
    int lw = 0, brk = -1;   // brk: index of the last space in line
    auto emit = [&] {
        std::u32string l = line;
        while (!l.empty() && l.back() == U' ') l.pop_back();
        out.push_back(to_utf8(l));
        line.clear();
        lw = 0;
        brk = -1;
    };
    for (char32_t c : from_utf8(clean(s))) {
        if (c == U'\n') { emit(); continue; }
        const int cw = cp_width(c);
        if (cw == 0) continue;
        if (lw + cw > width) {
            if (c == U' ') { emit(); continue; }
            if (brk > 0) {
                std::u32string rest = line.substr(static_cast<size_t>(brk) + 1);
                line.resize(static_cast<size_t>(brk));
                emit();
                line = rest;
                for (char32_t r : line) lw += cp_width(r);
            } else {
                emit();
            }
            if (lw + cw > width) emit();
        }
        line.push_back(c);
        lw += cw;
        if (c == U' ') brk = static_cast<int>(line.size()) - 1;
    }
    emit();
    while (out.size() > 1 && out.back().empty()) out.pop_back();
    if (max_rows > 0 && static_cast<int>(out.size()) > max_rows) {
        out.resize(static_cast<size_t>(max_rows));
        out.back() = cut(out.back(), width - 1) + "…";
    }
    return out;
}

std::string clip(const std::string& s, int width) {
    std::string one = clean(s);
    std::replace(one.begin(), one.end(), '\n', ' ');
    if (text_width(one) <= width) return one;
    if (width <= 0) return "";
    return cut(one, width - 1) + "…";
}

std::string sgr(const Style& st, bool truecolor) {
    std::string s = "\x1b[0";
    if (st.at & kBold) s += ";1";
    if (st.at & kDim) s += ";2";
    if (st.at & kItalic) s += ";3";
    if (st.at & kUnder) s += ";4";
    auto col = [&](uint32_t c, bool fg) {
        if (c == kNone) return;
        const unsigned r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
        if (truecolor)
            s += std::string(fg ? ";38;2;" : ";48;2;") + std::to_string(r) + ";" + std::to_string(g) + ";" + std::to_string(b);
        else
            s += std::string(fg ? ";38;5;" : ";48;5;") + std::to_string(to256(r, g, b));
    };
    col(st.fg, true);
    col(st.bg, false);
    s += 'm';
    return s;
}

// ---------------- Canvas ----------------

Canvas::Canvas(int w, int h)
    : w_(std::max(0, w)), h_(std::max(0, h)),
      cells_(static_cast<size_t>(std::max(0, w)) * static_cast<size_t>(std::max(0, h))) {}

void Canvas::put(int x, int y, char32_t ch, Style st) {
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
    int cw = cp_width(ch);
    if (cw == 0) return;
    if (cw == 2 && x + 1 >= w_) { ch = U' '; cw = 1; }
    Cell& c = ref(x, y);
    if (c.ch == 0 && x > 0) ref(x - 1, y).ch = U' ';                 // split a wide glyph's right half
    if (cp_width(c.ch) == 2 && x + 1 < w_) ref(x + 1, y).ch = U' ';  // overwrote its left half
    c = Cell{ch, st};
    if (cw == 2) {
        Cell& r = ref(x + 1, y);
        if (cp_width(r.ch) == 2 && x + 2 < w_) ref(x + 2, y).ch = U' ';
        r = Cell{0, st};
    }
}

int Canvas::text(int x, int y, const std::string& s, Style st, int max_w) {
    const int limit = max_w < 0 ? w_ : std::min(w_, x + max_w);
    int cx = x;
    for (size_t i = 0; i < s.size();) {
        char32_t c = decode(s, i);
        if (c == U'\t') c = U' ';
        const int cw = cp_width(c);
        if (cw == 0) continue;
        if (cx + cw > limit) break;
        put(cx, y, c, st);
        cx += cw;
    }
    return cx - x;
}

int Canvas::gradient(int x, int y, const std::string& s, uint32_t a, uint32_t b, uint32_t c, uint32_t at) {
    const int total = std::max(1, text_width(s) - 1);
    int cx = x;
    for (size_t i = 0; i < s.size();) {
        const char32_t ch = decode(s, i);
        const int cw = cp_width(ch);
        if (cw == 0) continue;
        if (cx + cw > w_) break;
        put(cx, y, ch, Style{tri(a, b, c, static_cast<double>(cx - x) / total), kNone, at});
        cx += cw;
    }
    return cx - x;
}

void Canvas::fill(int x, int y, int w, int h, Style st, char32_t ch) {
    const int x0 = std::max(0, x), x1 = std::min(w_, x + w);
    const int y0 = std::max(0, y), y1 = std::min(h_, y + h);
    if (x0 >= x1) return;
    const Cell c{ch, st};
    for (int j = y0; j < y1; ++j) {
        if (ref(x0, j).ch == 0 && x0 > 0) ref(x0 - 1, j).ch = U' ';
        if (cp_width(ref(x1 - 1, j).ch) == 2 && x1 < w_) ref(x1, j).ch = U' ';
        Cell* row = &ref(x0, j);
#pragma omp simd
        for (int i = 0; i < x1 - x0; ++i) row[i] = c;
    }
}

void Canvas::box(int x, int y, int w, int h, Style b, bool rounded) {
    if (w < 2 || h < 2) return;
    for (int i = 1; i < w - 1; ++i) {
        put(x + i, y, U'─', b);
        put(x + i, y + h - 1, U'─', b);
    }
    for (int j = 1; j < h - 1; ++j) {
        put(x, y + j, U'│', b);
        put(x + w - 1, y + j, U'│', b);
    }
    put(x, y, rounded ? U'╭' : U'┌', b);
    put(x + w - 1, y, rounded ? U'╮' : U'┐', b);
    put(x, y + h - 1, rounded ? U'╰' : U'└', b);
    put(x + w - 1, y + h - 1, rounded ? U'╯' : U'┘', b);
}

int Canvas::title(int x, int y, const std::string& label, Style label_st, Style border) {
    put(x, y, U'┤', border);
    put(x + 1, y, U' ', label_st);
    const int n = text(x + 2, y, label, label_st);
    put(x + 2 + n, y, U' ', label_st);
    put(x + 3 + n, y, U'├', border);
    return n + 4;
}

int Canvas::title_right(int right, int y, const std::string& label, Style label_st, Style border) {
    return title(right - (text_width(label) + 4), y, label, label_st, border);
}

void Canvas::blit(const Canvas& src, int sy, int dx, int dy, int rows) {
    const int cols = std::min(src.w(), w_ - dx);
    if (cols <= 0) return;
    for (int r = 0; r < rows; ++r) {
        const int ty = dy + r, fy = sy + r;
        if (ty < 0 || ty >= h_ || fy < 0 || fy >= src.h()) continue;
        for (int i = 0; i < cols; ++i) ref(dx + i, ty) = src.at(i, fy);
        if (cp_width(ref(dx + cols - 1, ty).ch) == 2 && (dx + cols >= w_ || cols < src.w()))
            ref(dx + cols - 1, ty).ch = U' ';
    }
}

std::string Canvas::row_text(int y) const {
    std::string o;
    if (y < 0 || y >= h_) return o;
    for (int x = 0; x < w_; ++x)
        if (at(x, y).ch != 0) encode(o, at(x, y).ch);
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o;
}

std::string Canvas::to_ansi(bool color, bool truecolor) const {
    std::string o;
    for (int y = 0; y < h_; ++y) {
        int end = w_;
        while (end > 0 && at(end - 1, y).ch == U' ' && at(end - 1, y).st.bg == kNone) --end;
        Style cur;
        bool styled = false;
        for (int x = 0; x < end; ++x) {
            const Cell& c = at(x, y);
            if (c.ch == 0) continue;
            if (color && (!styled || !(c.st == cur))) {
                o += sgr(c.st, truecolor);
                cur = c.st;
                styled = true;
            }
            encode(o, c.ch);
        }
        if (styled) o += "\x1b[0m";
        o += '\n';
    }
    return o;
}

// ---------------- Frame ----------------

Frame::Frame(int w, int h) : Canvas(w, h) {}

// Pass 1 diffs the grid with the widest SIMD unit; pass 2 encodes the dirty
// rows, each self-contained (own cursor move + SGR), so rows go to OpenMP
// threads when a frame is big and are joined in order.
std::string Frame::present(bool truecolor) {
    std::string head;
    if (full_ || prev_.size() != cells_.size()) {
        prev_.assign(cells_.size(), Cell{kSentinel, Style{}});
        head = "\x1b[0m\x1b[2J";
        full_ = false;
    }
    const size_t n = cells_.size();
    std::vector<char> dirty(n);
    std::vector<int> per_row(static_cast<size_t>(h_));
    int total = 0;
#pragma omp parallel for schedule(static) reduction(+ : total) if (n >= kParCells) num_threads(par_threads())
    for (int y = 0; y < h_; ++y) {
        per_row[static_cast<size_t>(y)] = simd::diff_cells(&cells_[idx(0, y)], &prev_[idx(0, y)], w_, &dirty[idx(0, y)]);
        total += per_row[static_cast<size_t>(y)];
    }

    std::vector<std::string> rows(static_cast<size_t>(h_));
#pragma omp parallel for schedule(dynamic, 2) if (total >= kParDirty) num_threads(par_threads())
    for (int y = 0; y < h_; ++y) {
        if (per_row[static_cast<size_t>(y)] == 0) continue;
        char* d = &dirty[idx(0, y)];
        for (int x = 1; x < w_; ++x)
            if (d[x] && at(x, y).ch == 0) d[x - 1] = 1;   // a wide glyph repaints from its left half
        std::string& out = rows[static_cast<size_t>(y)];
        out.reserve(static_cast<size_t>(per_row[static_cast<size_t>(y)]) * 24);
        int cx = -1;
        Style cur;
        bool styled = false;
        for (int x = 0; x < w_; ++x) {
            if (!d[x]) continue;
            const Cell& c = at(x, y);
            if (c.ch == 0) continue;
            if (cx != x) out += "\x1b[" + std::to_string(y + 1) + ";" + std::to_string(x + 1) + "H";
            if (!styled || !(c.st == cur)) {
                out += sgr(c.st, truecolor);
                cur = c.st;
                styled = true;
            }
            encode(out, c.ch);
            cx = x + cp_width(c.ch);
            if (cx >= w_) cx = -1;   // pending wrap: position explicitly next time
        }
        if (styled) out += "\x1b[0m";
    }
    std::memcpy(prev_.data(), cells_.data(), n * sizeof(Cell));

    size_t bytes = head.size();
    for (const auto& r : rows) bytes += r.size();
    std::string out;
    out.reserve(bytes);
    out += head;
    for (const auto& r : rows) out += r;
    return out;
}

// ---------------- Theme ----------------

Theme Theme::load(const std::string& path) {
    Theme t;
    core::Config c{core::Json{}};
    try {
        c = core::load_config(path);
    } catch (...) {
        return t;
    }
    auto col = [&](const char* key, uint32_t& dst) {
        const std::string v = c.get_string(std::string("colors.") + key);
        if (v.size() != 7 || v[0] != '#') return;
        char* end = nullptr;
        const unsigned long hex = std::strtoul(v.c_str() + 1, &end, 16);
        if (end && *end == '\0') dst = rgb(static_cast<uint32_t>(hex));
    };
    col("banner_border", t.border);
    col("banner_title", t.title);
    col("banner_accent", t.accent);
    col("banner_dim", t.dim);
    col("banner_text", t.text);
    col("ui_accent", t.hot);
    col("ui_label", t.label);
    col("ui_ok", t.ok);
    col("ui_error", t.error);
    col("ui_warn", t.warn);
    col("ui_gold", t.gold);
    col("prompt", t.prompt);
    col("input_rule", t.input_rule);
    col("response_border", t.response);
    col("session_label", t.session_label);
    col("session_border", t.session_border);
    col("status_bar_bg", t.status_bg);
    col("completion_menu_current_bg", t.select_bg);
    t.muted = mix(t.text, t.dim, 0.55);

    if (const core::Json* w = c.get_list("spinner.wings"); w && w->type == core::Json::Arr) {
        std::vector<std::pair<std::string, std::string>> wings;
        for (const auto& p : w->arr)
            if (p && p->type == core::Json::Arr && p->arr.size() == 2 && p->arr[0] && p->arr[1])
                wings.emplace_back(p->arr[0]->str, p->arr[1]->str);
        if (!wings.empty()) t.wings = std::move(wings);
    }
    if (const core::Json* v = c.get_list("spinner.thinking_verbs"); v && v->type == core::Json::Arr) {
        std::vector<std::string> verbs;
        for (const auto& p : v->arr)
            if (p && p->type == core::Json::Str && !p->str.empty()) verbs.push_back(p->str);
        if (!verbs.empty()) t.verbs = std::move(verbs);
    }
    auto str = [&](const char* key, std::string& dst) {
        const std::string v = trim(c.get_string(std::string("branding.") + key));
        if (!v.empty()) dst = v;
    };
    str("agent_name", t.agent_name);
    str("response_label", t.response_label);
    str("welcome", t.welcome);
    str("goodbye", t.goodbye);
    if (const std::string sym = c.get_string("branding.prompt_symbol"); !sym.empty()) t.prompt_symbol = sym;
    return t;
}

// ---------------- Scene: feed side (any thread) ----------------

Scene::Scene(Theme th) : th_(std::move(th)) {}

// A lifecycle event, published into a fixed slot: one release store, claimed by whoever renders.
// The stamp is taken here, not inside the ring, so it shares the counter with the stream buffers.
void Scene::post(Event e) {
    e.at = clock::now();
    e.stamp = order_.fetch_add(1, std::memory_order_relaxed) + 1;
    q_.publish_any(std::move(e));   // the refusal is counted on the ring; dropped_events() reads it
}

// The streaming thread's side. The buffer has one writer, so appending is a plain copy — no CAS,
// no node, no allocation. The one rule is ownership of bytes: a byte may be written where the
// reader cannot yet reach it, or where the reader has already passed it, never in between. Past
// that the delta is refused and counted, because silently rewriting a byte the render thread is
// reading is the bug this whole wave exists to remove.
void Scene::stream(LiveBuf& b, const std::string& delta) {
    if (delta.empty()) return;
    const uint64_t stamp = order_.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint64_t len = b.len.load(std::memory_order_relaxed);   // ours: this thread is len's only writer
    const uint64_t read = b.read.load(std::memory_order_acquire);
    if (len - read + delta.size() > b.kCap) {
        dropped_live_.fetch_add(delta.size(), std::memory_order_relaxed);
        return;
    }
    if (len == read) {   // nothing pending: this delta starts a run, so its clock and count start here
        b.run_start.store(stamp, std::memory_order_relaxed);
        b.first_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count(),
                         std::memory_order_relaxed);
        b.run_tokens.store(0, std::memory_order_relaxed);   // the count is per run; the card adds runs
    }
    const size_t head = static_cast<size_t>(len % b.kCap);
    const size_t first = std::min(delta.size(), b.kCap - head);
    std::memcpy(b.bytes.data() + head, delta.data(), first);
    if (first < delta.size()) std::memcpy(b.bytes.data(), delta.data() + first, delta.size() - first);
    b.run_tokens.fetch_add(approx_tokens(delta), std::memory_order_relaxed);
    b.run_end.store(stamp, std::memory_order_relaxed);
    b.len.store(len + delta.size(), std::memory_order_release);   // publishes the bytes, the stamps, the clock, the count
}

void Scene::set_info(const Info& info) {
    Event e;
    e.ev = Ev::Info;
    e.info = std::make_shared<const Info>(info);
    post(std::move(e));
}

void Scene::set_max_turns(int n) {
    Event e;
    e.ev = Ev::MaxTurns;
    e.token = static_cast<uint64_t>(std::max(0, n));
    post(std::move(e));
}

void Scene::user(const std::string& text) { Event e; e.ev = Ev::User; e.a = text; post(std::move(e)); }
void Scene::say(const std::string& text) { Event e; e.ev = Ev::Say; e.a = text; post(std::move(e)); }
void Scene::note(const std::string& text) { Event e; e.ev = Ev::Note; e.a = text; post(std::move(e)); }
void Scene::recalled(const RecallView& v) {
    Event e;
    e.ev = Ev::Memory;
    e.recall = std::make_shared<const RecallView>(v);
    post(std::move(e));
}
void Scene::error(const std::string& text) { Event e; e.ev = Ev::Error; e.a = text; post(std::move(e)); }
void Scene::clear() { Event e; e.ev = Ev::Clear; post(std::move(e)); }

// A new call is an event like any other. The stream buffers need no reset: a run begins when the
// buffer has been drained, and llm_begin's stamp orders itself against the runs around it. Touching
// the buffer's words from here would put a second writer on words the streaming thread owns.
void Scene::llm_begin() {
    Event e;
    e.ev = Ev::LlmBegin;
    post(std::move(e));
}
void Scene::turn_begin() { Event e; e.ev = Ev::TurnBegin; post(std::move(e)); }
void Scene::turn_end() { Event e; e.ev = Ev::TurnEnd; post(std::move(e)); }

void Scene::llm_reasoning(const std::string& delta) { stream(reason_, delta); }
void Scene::llm_content(const std::string& delta) { stream(say_, delta); }
void Scene::set_effort(const std::string& label) { Event e; e.ev = Ev::Effort; e.a = label; post(std::move(e)); }
void Scene::recall(bool active) { Event e; e.ev = Ev::Recall; e.ok = active; post(std::move(e)); }

void Scene::set_tape(const TapeView& t) {
    Event e;
    e.ev = Ev::Tape;
    e.tape = std::make_shared<const TapeView>(t);
    post(std::move(e));
}

void Scene::set_plan(const PlanView& p) {
    Event e;
    e.ev = Ev::Plan;
    e.plan = std::make_shared<const PlanView>(p);
    post(std::move(e));
}

void Scene::llm_end(const LlmStats& s) {
    Event e;
    e.ev = Ev::LlmEnd;
    e.llm = s;
    post(std::move(e));
}

void Scene::tool_call(const std::string& id, const std::string& name, const std::string& args) {
    Event e;
    e.ev = Ev::ToolCall;
    e.a = args;
    e.b = name;
    e.c = id;
    post(std::move(e));
}

uint64_t Scene::tool_begin(const std::string& name) {
    Event e;
    e.ev = Ev::ToolBegin;
    e.b = name;
    e.token = next_token_.fetch_add(1, std::memory_order_relaxed);
    const uint64_t tok = e.token;
    post(std::move(e));
    return tok;
}

void Scene::tool_end(uint64_t token, double ms, bool ok) {
    Event e;
    e.ev = Ev::ToolEnd;
    e.token = token;
    e.ms = ms;
    e.ok = ok;
    post(std::move(e));
}

void Scene::tool_result(const std::string& id, const std::string& text) {
    Event e;
    e.ev = Ev::ToolResult;
    e.a = text;
    e.c = id;
    post(std::move(e));
}

// ---------------- Scene: render side ----------------

Entry& Scene::push(Kind k, const std::string& text) {
    if (entries_.size() >= 400) {
        entries_.pop_front();
        cache_.pop_front();
        if (printed_ > 0) --printed_;
    }
    Entry e;
    e.kind = k;
    e.text = text.size() > 64000 ? text.substr(0, 64000) : text;
    e.clock = hhmmss();
    entries_.push_back(std::move(e));
    cache_.emplace_back();
    return entries_.back();
}

void Scene::apply() {
    // The two channels are merged by feed order, not drained one after the other. Draining the
    // streams last would put every streamed byte after llm_end, so a call that streamed and then
    // ended would render its reasoning as still live — exactly what the suite caught. Draining
    // first would put it before llm_begin, which closes the previous call's cards.
    for (Event& e : q_.claim_all()) {
        flush_before(e.stamp);   // every run the producer stamped before this event
        apply(e);
    }
    flush_before(std::numeric_limits<uint64_t>::max());   // whatever arrived after the last event
}

// Flush whole runs, oldest first, up to (not including) `stamp`. A run is a stretch of bytes the
// streaming thread published between two events; the render thread cannot take half of it, because
// the card's clock, token count and feed order were published with the run's last byte.
void Scene::flush_before(uint64_t stamp) {
    for (;;) {
        const uint64_t len_r = reason_.len.load(std::memory_order_acquire);
        const uint64_t len_s = say_.len.load(std::memory_order_acquire);
        const bool r = len_r > reason_.read.load(std::memory_order_relaxed);
        const bool s = len_s > say_.read.load(std::memory_order_relaxed);
        if (r && reason_.run_end.load(std::memory_order_relaxed) < stamp && (!s || reason_.run_start.load(std::memory_order_relaxed) < say_.run_start.load(std::memory_order_relaxed)))
            flush_run(reason_, Kind::Reason, live_reason_);
        else if (s && say_.run_end.load(std::memory_order_relaxed) < stamp)
            flush_run(say_, Kind::Say, live_say_);
        else return;   // nothing pending is older than the event we are about to apply
    }
}

// The render thread's side of a stream buffer: take the published bytes, append them to the card,
// then move the read cursor so the writer may reuse them. The cursor is ours to write and the
// length is ours to read, so the two threads never touch the same byte at the same time — the
// writer's guard is exactly `len - read`, and we can only grow that gap by not running.
void Scene::flush_run(LiveBuf& b, Kind kind, Entry*& live) {
    const uint64_t len = b.len.load(std::memory_order_acquire);
    const uint64_t read = b.read.load(std::memory_order_relaxed);   // ours: read's only writer
    if (len == read) return;

    const uint64_t first_ns = b.first_ns.load(std::memory_order_relaxed);
    const clock::time_point first = first_ns ? clock::time_point(std::chrono::nanoseconds(first_ns)) : clock::now();

    std::string text;
    const size_t n = static_cast<size_t>(len - read);
    const size_t head = static_cast<size_t>(read % b.kCap);
    const size_t first_run = std::min(n, b.kCap - head);
    text.assign(b.bytes.data() + head, first_run);
    if (first_run < n) text.append(b.bytes.data(), n - first_run);

    if (kind == Kind::Reason) {
        if (!live) {
            live = &push(Kind::Reason, "");
            reason_t0_ = first;
        }
        live->text += text;
        cap_live(live->text);
        live->n += static_cast<long>(b.run_tokens.load(std::memory_order_relaxed));
        ++live->ver;
        phase_ = Phase::Reasoning;
    } else {
        if (live_reason_ && !live_reason_->resolved) {   // thinking is over once the answer starts
            live_reason_->ms = std::chrono::duration<double, std::milli>(first - reason_t0_).count();
            live_reason_->resolved = true;
            ++live_reason_->ver;
        }
        if (!live) live = &push(Kind::Say, "");
        live->text += text;
        cap_live(live->text);
        live->n += static_cast<long>(b.run_tokens.load(std::memory_order_relaxed));
        ++live->ver;
        phase_ = Phase::Writing;
    }
    b.read.store(len, std::memory_order_release);
}

void Scene::apply(Event& e) {
    auto tools_back = [&](auto pred) -> Entry* {
        for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
            if (it->kind == Kind::Tool && pred(*it)) return &*it;
        return nullptr;
    };
    switch (e.ev) {
    case Ev::Info:
        if (e.info) info_ = *e.info;
        break;
    case Ev::MaxTurns:
        info_.max_turns = static_cast<int>(e.token);
        break;
    case Ev::User:
        push(Kind::User, e.a);
        break;
    case Ev::Say: {
        Entry& t = push(Kind::Say, e.a);
        t.ms = last_.latency_ms;
        t.resolved = true;
        break;
    }
    case Ev::Effort:
        info_.effort = e.a;
        break;
    case Ev::Tape:
        if (e.tape) tape_ = *e.tape;
        break;
    case Ev::Plan:
        if (e.plan) plan_ = *e.plan;
        break;
    case Ev::Recall:
        if (e.ok) {
            phase_ = Phase::Recalling;
            phase_t0_ = e.at;
        } else if (phase_ == Phase::Recalling) {
            phase_ = Phase::Thinking;
            phase_t0_ = e.at;
        }
        break;
    case Ev::Note:
        push(Kind::Note, e.a);
        break;
    case Ev::Memory: {
        Entry& t = push(Kind::Memory, "");
        t.recall = e.recall;
        t.resolved = true;
        break;
    }
    case Ev::Error:
        push(Kind::Error, e.a);
        break;
    case Ev::Clear:
        entries_.clear();
        cache_.clear();
        live_reason_ = live_say_ = nullptr;
        printed_ = 0;
        scroll_ = 0;
        break;
    case Ev::ToolCall: {
        Entry& t = push(Kind::Tool, e.a);
        t.name = e.b;
        t.call_id = e.c;
        break;
    }
    case Ev::ToolBegin: {
        // earliest pending call of that name in this turn
        Entry* hit = nullptr;
        for (auto it = entries_.rbegin(); it != entries_.rend() && it->kind != Kind::User; ++it)
            if (it->kind == Kind::Tool && it->name == e.b && it->run == Run::Pending && it->token == 0) hit = &*it;
        if (hit) {
            hit->run = Run::Running;
            hit->token = e.token;
            ++hit->ver;
        }
        phase_ = Phase::Tools;
        phase_t0_ = e.at;
        running_tool_ = e.b;
        break;
    }
    case Ev::ToolEnd:
        if (Entry* t = tools_back([&](const Entry& x) { return x.token == e.token; })) {
            t->run = e.ok ? Run::Ok : Run::Failed;
            t->ms = e.ms;
            ++t->ver;
        }
        ++tools_turn_;
        break;
    case Ev::ToolResult:
        if (Entry* t = tools_back([&](const Entry& x) { return x.call_id == e.c && !x.resolved; })) {
            t->result = e.a.size() > 16000 ? e.a.substr(0, 16000) : e.a;
            t->resolved = true;
            if (t->run == Run::Pending || t->run == Run::Running)
                t->run = failed_result(t->result) ? Run::Failed : Run::Ok;
            ++t->ver;
        }
        break;
    case Ev::LlmBegin:
        close_live(e.at, 0);
        phase_ = Phase::Thinking;
        phase_t0_ = e.at;
        running_tool_.clear();
        break;
    case Ev::LlmEnd:
        // The endpoint's number is the fact: it replaces the estimate on the card this call wrote,
        // and the turn keeps a running output total so a later call adds to it instead of resetting.
        if (live_say_ && e.llm.completion_tokens > 0) live_say_->n = e.llm.completion_tokens;
        turn_out_ += e.llm.completion_tokens;
        close_live(e.at, e.llm.latency_ms);
        last_ = e.llm;
        ++calls_total_;
        ++turn_calls_;
        if (e.llm.gen_tps > 0) {
            tps_hist_.push_back(e.llm.gen_tps);
            if (tps_hist_.size() > 24) tps_hist_.pop_front();
        }
        break;
    case Ev::TurnBegin:
        busy_ = true;
        ++turns_;
        turn_calls_ = tools_turn_ = 0;
        turn_out_ = 0;
        turn_t0_ = phase_t0_ = e.at;
        phase_ = Phase::Thinking;
        scroll_ = 0;
        break;
    case Ev::TurnEnd: {
        if (!busy_) break;
        busy_ = false;
        close_live(e.at, 0);   // a stream cut by an interrupt
        // calls the loop skipped (interrupt) never get a result message
        for (auto it = entries_.rbegin(); it != entries_.rend() && it->kind != Kind::User; ++it)
            if (it->kind == Kind::Tool && !it->resolved) {
                it->resolved = true;
                it->run = Run::Failed;
                it->result = "{\"error\":\"cancelled\"}";
                ++it->ver;
            }
        phase_ = Phase::Idle;
        last_turn_ms_ = std::chrono::duration<double, std::milli>(e.at - turn_t0_).count();
        std::string r = "turn " + std::to_string(turns_) + " · " + std::to_string(turn_calls_) +
                        (turn_calls_ == 1 ? " call" : " calls");
        if (tools_turn_) r += " · " + std::to_string(tools_turn_) + (tools_turn_ == 1 ? " tool" : " tools");
        push(Kind::Rule, r + " · " + fmt_ms(last_turn_ms_));
        break;
    }
    }
}

void Scene::close_live(clock::time_point at, double latency_ms) {
    if (live_reason_ && !live_reason_->resolved) {
        live_reason_->ms = std::chrono::duration<double, std::milli>(at - reason_t0_).count();
        live_reason_->resolved = true;
        ++live_reason_->ver;
    }
    if (live_say_) {
        live_say_->resolved = true;
        if (latency_ms > 0) live_say_->ms = latency_ms;
        ++live_say_->ver;
    }
    live_reason_ = live_say_ = nullptr;
}

void Scene::set_input(const std::u32string& line, size_t cursor, const std::string& hint) {
    input_ = line;
    cursor_ = std::min(cursor, line.size());
    hint_ = hint;
}

void Scene::scroll(int rows) { scroll_ = std::max(0, scroll_ + rows); }

double Scene::work_ms() const {
    if (!busy_) return last_turn_ms_;
    return std::chrono::duration<double, std::milli>(clock::now() - turn_t0_).count();
}

bool Scene::stale(size_t i, int w) const {
    const Cache& c = cache_[i];
    const Entry& e = entries_[i];
    if (c.tw != static_cast<int>(e.text.size())) return true;   // streamed text grew
    const int spin = e.kind == Kind::Tool && e.run == Run::Running ? tick_ % 10 : -1;
    const int view = e.kind == Kind::Reason ? static_cast<int>(view_) : -1;
    return c.w != w || c.ver != e.ver || c.spin != spin || c.view != view || c.verb != static_cast<int>(verb_);
}

const Canvas& Scene::block(size_t i, int w) {
    Cache& c = cache_[i];
    if (stale(i, w)) {
        const Entry& e = entries_[i];
        c.block = render(e, w);
        c.w = w;
        c.tw = static_cast<int>(e.text.size());
        c.ver = e.ver;
        c.spin = e.kind == Kind::Tool && e.run == Run::Running ? tick_ % 10 : -1;
        c.view = e.kind == Kind::Reason ? static_cast<int>(view_) : -1;
        c.verb = static_cast<int>(verb_);
    }
    return c.block;
}

Canvas Scene::render(const Entry& e, int w) const {
    switch (e.kind) {
    case Kind::User: {
        const auto lines = wrap(e.text, w - 3, verb_ == Verbosity::Minimal ? 4 : verb_ == Verbosity::Verbose ? 30 : 10);
        Canvas c(w, static_cast<int>(lines.size()) + 2);
        const Style bar{th_.gold};
        c.put(0, 0, U'▍', bar);
        const int n = c.text(2, 0, "you", Style{th_.gold, kNone, kBold});
        c.text(3 + n, 0, "· " + e.clock, Style{th_.muted});
        for (size_t k = 0; k < lines.size(); ++k) {
            c.put(0, static_cast<int>(k) + 1, U'▍', bar);
            c.text(2, static_cast<int>(k) + 1, lines[k], Style{th_.text});
        }
        return c;
    }
    case Kind::Say: {
        // dynamic height: the box grows with the stream as it writes (capped),
        // then shrinks to the wrapped answer once resolved.
        const int cap = e.resolved ? 0 : (verb_ == Verbosity::Minimal ? 8 : verb_ == Verbosity::Verbose ? 64 : 24);
        const auto lines = wrap(e.text, w - 4, cap);
        const int h = static_cast<int>(lines.size()) + 3;
        Canvas c(w, h);
        const Style b{th_.response};
        c.box(0, 0, w, h - 1, b, true);
        c.put(2, 0, U'┤', b);
        c.put(3, 0, U' ', b);
        const int n = c.gradient(4, 0, th_.response_label, th_.title, th_.hot, th_.gold, kBold);
        c.put(4 + n, 0, U' ', b);
        c.put(5 + n, 0, U'├', b);
        if (e.ms > 0) c.title_right(w - 2, 0, fmt_ms(e.ms), Style{th_.muted}, b);
        for (size_t k = 0; k < lines.size(); ++k) c.text(2, static_cast<int>(k) + 1, lines[k], Style{th_.text});
        return c;
    }
    case Kind::Tool: {
        // dynamic height: arguments and result previews are capped by verbosity:
        // quiet shows a glance, verbose opens what the call actually carried. Both caps
        // announce themselves -- a card that hides its own cut tells the operator the
        // tool produced what the card shows.
        const bool verbose = verb_ == Verbosity::Verbose;
        const int aw = std::clamp(w - 4, 40, verbose ? 112 : 96);
        // flat_args first (it repairs a cut-off JSON blob into key="v" form),
        // then wrap: long args get their rows instead of one clipped line.
        const int arg_rows = verb_ == Verbosity::Minimal ? 2 : verbose ? 20 : 6;
        const int arg_w = verbose ? 240 : verb_ == Verbosity::Minimal ? 40 : 60;
        auto alines = wrap(flat_args(e.text, arg_w), aw, arg_rows);
        while (alines.size() > 1 && alines.front().empty()) alines.erase(alines.begin());
        bool err = false;
        ResultFacts f;
        std::vector<std::string> rlines;
        long rows_hidden = 0, acct_hidden = 0;
        std::string acct;
        std::vector<std::string> acct_rows;
        if (e.resolved) {
            f = result_facts(e.result);
            err = f.err;
            const int res_rows = verb_ == Verbosity::Minimal ? 3 : verbose ? 48 : 12;
            rlines = wrap_rows(f.body.empty() ? std::string("(empty)") : f.body, w - 6, res_rows, rows_hidden);
            acct = verbose ? result_account(f) : std::string();
            // The account wraps rather than clips: a single row cut mid-sentence reads as
            // a complete sentence, which is exactly the claim this card exists to retract.
            if (!acct.empty()) acct_rows = wrap_rows(acct, w - 6, 2, acct_hidden);
        }
        // rows the card did not show, plus the tool's own accounting, when each has something to say
        const int extra = (rows_hidden > 0 ? 1 : 0) + static_cast<int>(acct_rows.size()) + (acct_hidden > 0 ? 1 : 0);
        const int h = static_cast<int>(alines.size()) + (e.resolved ? static_cast<int>(rlines.size()) + 1 : 1) + extra + 3;
        Canvas c(w, h);
        const uint32_t bc = e.run == Run::Failed ? th_.error : e.run == Run::Running ? th_.accent : th_.dim;
        const Style b{bc};
        c.box(0, 0, w, h - 1, b, false);
        c.put(1, 0, U' ', b);
        c.put(2, 0, U'▸', Style{th_.gold, kNone, kBold});
        c.put(3, 0, U' ', b);
        std::string st;
        Style ss{th_.muted};
        switch (e.run) {
        case Run::Pending: st = "queued"; break;
        case Run::Running: {
            std::string g;
            encode(g, kSpin[tick_ % 10]);
            st = g + " running";
            ss = Style{th_.hot};
            break;
        }
        case Run::Ok: st = "✓ " + (e.ms > 0 ? fmt_ms(e.ms) : std::string("done")); ss = Style{th_.ok}; break;
        case Run::Failed: st = "✗ " + (e.ms > 0 ? fmt_ms(e.ms) : std::string("failed")); ss = Style{th_.error}; break;
        }
        // Header budget: the status keeps its ground and the name gives way, never the
        // reverse. The old fixed name budget (w - 26) silently clipped a long status
        // onto the name, so truth lost characters first at narrow widths.
        const int max_sw = std::max(8, w - 34);
        // The tool's cut rides the status, so a folded card still says it was cut.
        if (e.resolved) {
            const std::string loss = loss_datum(f, verb_, std::max(0, max_sw - text_width(st) - 3));
            if (!loss.empty()) st += " · " + loss;
        }
        const int sw = text_width(st);   // "─ ✓ 1.2s · 978.3 KB dropped ┐"
        const int n = c.text(4, 0, e.name, Style{th_.accent, kNone, kBold}, std::max(8, w - sw - 10));
        c.put(4 + n, 0, U' ', b);
        c.put(w - sw - 3, 0, U' ', b);
        c.text(w - sw - 2, 0, st, ss);
        c.put(w - 2, 0, U' ', b);
        int ry = 1;
        for (size_t k = 0; k < alines.size(); ++k, ++ry) {
            if (alines[k].empty()) continue;
            c.text(2, ry, alines[k], Style{th_.muted});
        }
        if (e.resolved) {
            c.text(2, ry++, "◂", Style{err ? th_.error : th_.ok, kNone, kBold});
            for (size_t k = 0; k < rlines.size(); ++k)
                c.text(4, ry + static_cast<int>(k), rlines[k], Style{err ? th_.error : th_.text});
            ry += static_cast<int>(rlines.size());
            if (!acct_rows.empty()) {
                for (size_t k = 0; k < acct_rows.size(); ++k)
                    c.text(4, ry + static_cast<int>(k), acct_rows[k], Style{th_.muted});
                ry += static_cast<int>(acct_rows.size());
                if (acct_hidden > 0)
                    c.text(4, ry++, "… +" + group(acct_hidden) + " rows of accounting not shown",
                           Style{th_.muted, kNone, kDim});
            }
            if (rows_hidden > 0) {
                const std::string t = verbose ? "… +" + group(rows_hidden) + " rows past the verbose cap"
                                             : "… +" + group(rows_hidden) + " rows not shown (verbose opens them)";
                c.text(4, ry++, t, Style{th_.muted, kNone, kDim});
            }
        } else if (e.run == Run::Running) {
            c.text(2, ry, "◂ …", Style{th_.muted});
        }
        return c;
    }
    case Kind::Note: {
        const auto lines = wrap(e.text, w - 2, verb_ == Verbosity::Minimal ? 1 : verb_ == Verbosity::Verbose ? 40 : 16);
        Canvas c(w, static_cast<int>(lines.size()));
        c.put(0, 0, U'·', Style{th_.accent});
        for (size_t k = 0; k < lines.size(); ++k) c.text(2, static_cast<int>(k), lines[k], Style{th_.muted});
        return c;
    }
    case Kind::Error: {
        const auto lines = wrap(e.text, w - 4, verb_ == Verbosity::Minimal ? 3 : verb_ == Verbosity::Verbose ? 32 : 8);
        const int h = static_cast<int>(lines.size()) + 3;
        Canvas c(w, h);
        const Style b{th_.error};
        c.box(0, 0, w, h - 1, b, true);
        c.title(2, 0, "✗ error", Style{th_.error, kNone, kBold}, b);
        for (size_t k = 0; k < lines.size(); ++k) c.text(2, static_cast<int>(k) + 1, lines[k], Style{th_.error});
        return c;
    }
    case Kind::Reason: {
        // dashed, dim: thought, not output. Tail = fixed height, so a live
        // stream never moves the cards below it.
        const std::string head = e.resolved ? "◈ thought" : "◈ thinking";
        std::string meta = (e.resolved ? "" : "≈") + fmt_tok(e.n) + " tok";   // estimated until the call ends
        if (e.ms > 0) meta += " · " + fmt_ms(e.ms);
        const Style dim{th_.dim}, txt{th_.muted, kNone, kItalic};
        if (view_ == ReasonView::Hide || verb_ == Verbosity::Minimal) {
            Canvas c(w, 2);
            const int n = c.text(0, 0, head, Style{th_.accent, kNone, kItalic});
            c.text(n + 1, 0, "· " + meta + " · /reasoning show", txt);
            return c;
        }
        // dynamic height: Full shows every line; Tail grows with the stream
        // up to its cap and always keeps the NEWEST lines (cut the front).
        auto lines = wrap(e.text.empty() ? std::string("…") : e.text, w - 4);
        while (lines.size() > 1 && lines.front().empty()) lines.erase(lines.begin());
        while (lines.size() > 1 && lines.back().empty()) lines.pop_back();
        // how much the cap will hide has to be read off BEFORE the cut
        const size_t first = (view_ == ReasonView::Full || verb_ == Verbosity::Verbose || static_cast<int>(lines.size()) <= kReasonRows)
            ? 0
            : lines.size() - static_cast<size_t>(kReasonRows);
        if (first > 0) lines.erase(lines.begin(), lines.begin() + first);
        const int rows = static_cast<int>(lines.size());
        Canvas c(w, rows + 3);
        for (int i = 1; i < w - 1; ++i) {
            c.put(i, 0, U'╌', dim);
            c.put(i, rows + 1, U'╌', dim);
        }
        for (int j = 1; j <= rows; ++j) {
            c.put(0, j, U'┆', dim);
            c.put(w - 1, j, U'┆', dim);
        }
        c.put(0, 0, U'╭', dim);
        c.put(w - 1, 0, U'╮', dim);
        c.put(0, rows + 1, U'╰', dim);
        c.put(w - 1, rows + 1, U'╯', dim);
        c.title(2, 0, head, Style{th_.accent, kNone, kItalic | kBold}, dim);
        c.title_right(w - 2, 0, meta, Style{th_.muted}, dim);
        if (first > 0) c.title_right(w - 2, rows + 2, "↑ " + std::to_string(first) + " lines · /reasoning full", Style{th_.muted}, dim);
        for (size_t k = 0; k < lines.size(); ++k)
            c.text(2, 1 + static_cast<int>(k), lines[k], txt);
        return c;
    }
    case Kind::Memory: {
        // one row per memory the turn carries: what the model reads, by id
        const int kRows = verb_ == Verbosity::Minimal ? 6 : verb_ == Verbosity::Verbose ? 24 : 12;
        const RecallView empty;
        const RecallView& v = e.recall ? *e.recall : empty;
        const int extra = v.skill.empty() ? 0 : 1;
        const int shown = std::min(static_cast<int>(v.rows.size()), kRows - extra);
        const int more = static_cast<int>(v.rows.size()) - shown;
        const int h = shown + extra + (more > 0 ? 1 : 0) + 3;
        Canvas c(w, h);
        const Style b{th_.dim};
        c.box(0, 0, w, h - 1, b, true);
        const int tw = c.title(2, 0, "◈ mazemaker", Style{th_.gold, kNone, kBold}, b);
        if (const int room = w - 9 - tw; room >= 6) c.title_right(w - 2, 0, clip(v.meta, room), Style{th_.muted}, b);
        // fixed columns: glyph · #id · label · score · snippet
        int idw = 2;
        for (int k = 0; k < shown; ++k) idw = std::max(idw, 1 + static_cast<int>(std::to_string(v.rows[static_cast<size_t>(k)].id).size()));
        const int lw = std::clamp((w - 12) / 4, 10, 30);
        int y = 1;
        for (int k = 0; k < shown; ++k, ++y) {
            const MemoryRow& r = v.rows[static_cast<size_t>(k)];
            const char* glyph = r.kind == 'l' ? "↳" : r.kind == 'f' ? "∴" : r.kind == 'e' ? "⟲" : "◆";
            c.text(2, y, glyph, Style{r.kind == 'r' ? th_.gold : th_.accent, kNone, kBold});
            int x = 4;
            const std::string id = "#" + std::to_string(r.id);
            c.text(x + idw - text_width(id), y, id, Style{th_.gold});
            x += idw + 1;
            c.text(x, y, clip(r.label, lw), Style{th_.accent});
            x += lw + 1;
            if (r.score > 0) {
                char sc[16];
                std::snprintf(sc, sizeof sc, "%.2f", std::min(r.score, 9.99));
                c.text(x, y, sc, Style{th_.muted});
            }
            x += 5;
            if (x < w - 3) c.text(x, y, clip(r.text, w - 2 - x), Style{th_.text});
        }
        if (more > 0) c.text(4, y++, "+ " + std::to_string(more) + " more · /context", Style{th_.muted});
        if (extra) {
            const int n = c.text(2, y, "✦", Style{th_.gold, kNone, kBold});
            c.text(3 + n, y, clip("skill " + v.skill + " · skill_load(\"" + v.skill + "\")", w - 5 - n),
                   Style{th_.accent});
        }
        return c;
    }
    case Kind::Rule: {
        Canvas c(w, 2);
        const std::string label = " " + e.text + " ";
        c.fill(0, 0, w, 1, Style{th_.session_border}, U'─');
        c.text(std::max(0, (w - text_width(label)) / 2), 0, label, Style{th_.muted});
        return c;
    }
    }
    return Canvas(w, 1);
}

void Scene::paint_splash(Frame& f, int x, int y, int w, int h) const {
    const int logo_w = text_width(kLogo[0]);
    auto center = [&](int row, const std::string& s, Style st) {
        const std::string t = clip(s, w);
        f.text(x + std::max(0, (w - text_width(t)) / 2), row, t, st);
    };
    if (h < 10 || w < logo_w + 2) {
        center(y + h / 2, th_.welcome, Style{th_.accent, kNone, kItalic});
        return;
    }
    const int ly = y + std::max(0, (h - 10) / 2), lx = x + (w - logo_w) / 2;
    for (int r = 0; r < 3; ++r) {
        int cx = lx;
        for (size_t i = 0; kLogo[r][i];) {
            const char32_t ch = decode(kLogo[r], i);
            const double t = static_cast<double>(cx - lx) / std::max(1, logo_w - 1);
            f.put(cx++, ly + r, ch, Style{tri(th_.border, th_.title, th_.gold, t), kNone, kBold});
        }
    }
    center(ly + 4, "c++ agent harness  ·  " + (info_.model.empty() ? std::string("no model") : info_.model), Style{th_.muted});
    center(ly + 6, th_.welcome, Style{th_.accent, kNone, kItalic});
    center(ly + 8, "enter send  ·  esc interrupt  ·  ↑↓ history  ·  pgup/pgdn scroll  ·  /help", Style{th_.muted});
}

void Scene::paint_transcript(Frame& f, int x, int y, int w, int h) {
    const Style b{th_.border};
    f.box(x, y, w, h, b, true);
    f.put(x + 2, y, U'┤', b);
    f.put(x + 3, y, U' ', b);
    const int n = f.gradient(x + 4, y, "⟁ ARIADNE", th_.border, th_.title, th_.gold, kBold);
    f.put(x + 4 + n, y, U' ', b);
    f.put(x + 5 + n, y, U'├', b);
    f.title(x + 6 + n, y, "c++", Style{th_.muted}, b);

    const int ix = x + 2, iy = y + 1, iw = w - 4, ih = h - 2;
    if (ih <= 0 || iw <= 4) return;
    if (entries_.empty()) {
        paint_splash(f, ix, iy, iw, ih);
        return;
    }
    // cards fill the transcript box — no dead column band on wide terminals
    const int cw = std::max(40, iw);
    {   // a resize invalidates every card: wrap them on all cores
        std::vector<size_t> todo;
        for (size_t i = 0; i < entries_.size(); ++i)
            if (stale(i, cw)) todo.push_back(i);
        if (static_cast<int>(todo.size()) >= kParBlocks) {
            const long m = static_cast<long>(todo.size());
#pragma omp parallel for schedule(dynamic, 1) num_threads(par_threads())
            for (long k = 0; k < m; ++k) {
                const size_t i = todo[static_cast<size_t>(k)];
                const Entry& e = entries_[i];
                Cache& c = cache_[i];
                c.block = render(e, cw);
                c.w = cw;
                c.tw = static_cast<int>(e.text.size());
                c.ver = e.ver;
                c.spin = e.kind == Kind::Tool && e.run == Run::Running ? tick_ % 10 : -1;
                c.view = e.kind == Kind::Reason ? static_cast<int>(view_) : -1;
            }
        }
    }
    std::vector<int> hs(entries_.size());
    int total = 0;
    for (size_t i = 0; i < entries_.size(); ++i) total += hs[i] = block(i, cw).h();
    scroll_ = std::min(scroll_, std::max(0, total - ih));
    const int top = std::max(0, total - ih - scroll_);
    int acc = 0;
    for (size_t i = 0; i < entries_.size(); ++i) {
        const int bh = hs[i];
        const int a = std::max(acc, top), z = std::min(acc + bh, top + ih);
        if (z > a) f.blit(block(i, cw), a - acc, ix, iy + (a - top), z - a);
        acc += bh;
        if (acc >= top + ih) break;
    }
    if (total > ih) {
        const int thumb = std::max(1, ih * ih / total);
        const int pos = (ih - thumb) * top / std::max(1, total - ih);
        for (int r = 0; r < thumb; ++r) f.put(x + w - 1, iy + pos + r, U'┃', Style{th_.hot});
    }
    if (scroll_ > 0)
        f.title_right(x + w - 2, y, "↑ " + std::to_string(scroll_) + " rows · pgdn", Style{th_.warn}, b);
    else if (busy_)
        f.title_right(x + w - 2, y, "● live", Style{th_.ok}, b);
}

void Scene::paint_sidebar(Frame& f, int x, int y, int w, int h) const {
    const Style b{th_.session_border}, lab{th_.label}, val{th_.text}, mut{th_.muted};
    const Style head{th_.session_label, kNone, kBold};
    const int vx = x + 10, vw = w - 12;
    auto row = [&](int ry, const std::string& l, const std::string& v, Style vs) {
        f.text(x + 2, ry, l, lab);
        f.text(vx, ry, clip(v, vw), vs);
    };

    // verbose lowers the height gates so token, cache and epoch readouts survive
    // a short terminal; each panel needs its own box plus a border.
    const bool verbose = verb_ == Verbosity::Verbose;
    const int sh = 8;
    // a plan takes rows the moment one exists; verbose opens it wider
    const int pshown = plan_.items.empty() ? 0 : std::min<int>(static_cast<int>(plan_.items.size()), verbose ? 12 : 6);
    const int ph = plan_.items.empty() ? 0 : pshown + 2;
    const int rest = h - sh - ph;
    const bool engine = rest >= (verbose ? 15 : 20);
    const int eh = engine ? 7 : 0;
    const bool tape = tape_.on && rest - eh >= (verbose ? 8 : 21);
    const int tph = tape ? 7 : 0;
    const int th = rest - eh - tph;

    f.box(x, y, w, sh, b, true);
    f.title(x + 2, y, "session", head, b);
    std::string server = info_.endpoint;
    if (const size_t p = server.find("://"); p != std::string::npos) server = server.substr(p + 3);
    if (server.size() > 3 && server.compare(server.size() - 3, 3, "/v1") == 0) server.resize(server.size() - 3);
    row(y + 1, "model", info_.model, val);
    row(y + 2, "server", server, val);
    row(y + 3, "effort", info_.effort + (view_ == ReasonView::Hide ? " · hidden" : view_ == ReasonView::Full ? " · full" : ""),
        info_.effort == "off" ? mut : Style{th_.accent, kNone, kBold});
    row(y + 4, "turn", std::to_string(turn_calls_) + " / " + std::to_string(info_.max_turns) + " calls", val);
    row(y + 5, "session", std::to_string(turns_) + " turns · " + std::to_string(calls_total_) + " calls", val);
    row(y + 6, "work", fmt_ms(work_ms()), busy_ ? Style{th_.hot, kNone, kBold} : val);

    if (ph > 0) {   // the todo ledger: what the model said it would do, and where it stands
        const int py = y + sh;
        f.box(x, py, w, ph, b, true);
        f.title(x + 2, py, "plan · " + std::to_string(plan_.open) + " open of " + std::to_string(plan_.total), head, b);
        if (static_cast<int>(plan_.items.size()) > pshown)
            f.title_right(x + w - 2, py, "↑ " + std::to_string(static_cast<int>(plan_.items.size()) - pshown), mut, b);
        const size_t first = plan_.items.size() - static_cast<size_t>(pshown);   // newest last, oldest folded away
        int ry = py + 1;
        for (size_t i = first; i < plan_.items.size(); ++i) {
            const auto [mk, text] = plan_.items[i];
            const char32_t g = mk == PlanView::Active ? U'▶' : mk == PlanView::Done ? U'✓' : mk == PlanView::Dropped ? U'✗' : U'○';
            const Style gs = mk == PlanView::Active ? Style{th_.hot, kNone, kBold} : mk == PlanView::Done ? Style{th_.ok}
                           : mk == PlanView::Dropped ? Style{th_.dim} : mut;
            f.put(x + 2, ry, g, gs);
            f.text(x + 4, ry++, clip(text, w - 6), mk == PlanView::Active ? val : mut);
        }
    }

    if (engine) {
        const int ey = y + sh + ph;
        f.box(x, ey, w, eh, b, true);
        f.title(x + 2, ey, "engine", head, b);
        if (calls_total_ == 0) {
            f.text(x + 2, ey + 1, clip("waiting for the first call", w - 4), mut);
        } else {
            row(ey + 1, "gen", last_.gen_tps > 0 ? fmt1(last_.gen_tps) + " t/s" : "—", val);
            if (!tps_hist_.empty()) {   // sparkline, right-aligned
                static const char32_t bars[] = {U'▁', U'▂', U'▃', U'▄', U'▅', U'▆', U'▇', U'█'};
                const double mx = *std::max_element(tps_hist_.begin(), tps_hist_.end());
                const int n = std::min<int>(10, static_cast<int>(tps_hist_.size()));
                for (int i = 0; i < n; ++i) {
                    const double v = tps_hist_[tps_hist_.size() - static_cast<size_t>(n) + static_cast<size_t>(i)];
                    const int lvl = mx > 0 ? std::clamp(static_cast<int>(v / mx * 7.0 + 0.5), 0, 7) : 0;
                    f.put(x + w - 2 - n + i, ey + 1, bars[lvl], Style{tri(th_.dim, th_.title, th_.gold, lvl / 7.0)});
                }
            }
            row(ey + 2, "prefill", last_.pp_tps > 0 ? fmt1(last_.pp_tps) + " t/s" : "—", val);
            if (info_.n_ctx > 0) {
                const double frac = std::clamp(static_cast<double>(last_.prompt_tokens) / static_cast<double>(info_.n_ctx), 0.0, 1.0);
                const std::string pct = " " + std::to_string(static_cast<int>(frac * 100 + 0.5)) + "%";
                const int bw = vw - text_width(pct);
                f.text(x + 2, ey + 3, "ctx", lab);
                const int filled = static_cast<int>(frac * bw + 0.5);
                for (int i = 0; i < bw; ++i) {
                    const double t = static_cast<double>(i) / std::max(1, bw - 1);
                    if (i < filled) f.put(vx + i, ey + 3, U'▰', Style{tri(th_.ok, th_.warn, th_.error, t)});
                    else f.put(vx + i, ey + 3, U'▱', Style{th_.dim});
                }
                f.text(vx + bw, ey + 3, pct, val);
                f.text(vx, ey + 4, clip(fmt_tok(last_.prompt_tokens) + " / " + fmt_tok(info_.n_ctx), vw), mut);
            } else {
                row(ey + 3, "ctx", fmt_tok(last_.prompt_tokens) + " tok", val);
            }
            // The value column is 22 cells, so the forms are ranked rather than
            // appended: a clipped "800 c…" or "2 se…" reports a fact without saying
            // what it was. A re-sent call is the anomaly here, so the send count is
            // never the segment that loses — the cache readout is the expendable one,
            // and the spelled-out "sent N" rides the verbose status row.
            const std::string base = fmt_tok(last_.completion_tokens) + " tok · " + fmt_ms(last_.latency_ms);
            std::vector<std::string> forms;
            if (last_.attempts > 1) {
                const std::string sends = " · ×" + std::to_string(last_.attempts);
                if (last_.cached_tokens > 0) forms.push_back(base + " · " + fmt_tok(last_.cached_tokens) + " cached" + sends);
                forms.push_back(base + sends);
                forms.push_back(base);
            } else {
                if (last_.cached_tokens > 0) forms.push_back(base + " · " + fmt_tok(last_.cached_tokens) + " cached");
                forms.push_back(base);
            }
            std::string out;
            for (const auto& form : forms)
                if (text_width(form) <= vw) { out = form; break; }
            if (out.empty()) out = forms.back();   // nothing fits: show the bare call
            row(ey + 5, "last", out, val);
        }
    }

    if (tape) {
        const int py = y + sh + ph + eh;
        f.box(x, py, w, tph, b, true);
        f.title(x + 2, py, "tape · epoch " + std::to_string(tape_.epoch), head, b);
        if (!tape_.prep.empty())
            f.title_right(x + w - 2, py, tape_.prep == "ready" ? "next ready" : "warming",
                          Style{tape_.prep == "ready" ? th_.ok : th_.hot}, b);
        // the window against its rebase point
        if (tape_.rebase_at > 0) {
            const double frac = std::clamp(static_cast<double>(tape_.tokens) / static_cast<double>(tape_.rebase_at), 0.0, 1.0);
            const std::string tail = " " + fmt_tok(tape_.tokens) + "/" + fmt_tok(tape_.rebase_at);
            const int bw = std::max(4, vw - text_width(tail));
            f.text(x + 2, py + 1, "window", lab);
            const int filled = static_cast<int>(frac * bw + 0.5);
            for (int i = 0; i < bw; ++i) {
                const double t = static_cast<double>(i) / std::max(1, bw - 1);
                f.put(vx + i, py + 1, i < filled ? U'▰' : U'▱', i < filled ? Style{tri(th_.ok, th_.warn, th_.error, t)} : Style{th_.dim});
            }
            f.text(vx + bw, py + 1, tail, mut);
        } else {
            row(py + 1, "window", fmt_tok(tape_.tokens) + " tok · no rebase point", val);
        }
        if (tape_.prompt > 0) {
            const int pct = static_cast<int>(100.0 * static_cast<double>(tape_.reused) / static_cast<double>(tape_.prompt) + 0.5);
            row(py + 2, "reuse", std::to_string(pct) + "% of " + fmt_tok(tape_.prompt), pct >= 90 ? Style{th_.ok} : pct >= 50 ? Style{th_.warn} : Style{th_.error});
        } else {
            row(py + 2, "reuse", "—", mut);
        }
        // The sidebar's value column is 22 cells, so the widest form is the one that
        // fits — a clipped "2 s…" would report a fold without saying what it was, the
        // same lie a clipped card status was. The tape count is the expendable segment:
        // the fold count and the message count are the ones the operator reads.
        std::vector<std::string> hist_forms = {std::to_string(tape_.history) + " msgs · tape " + std::to_string(tape_.tape) +
                                               (tape_.steered ? " · " + std::to_string(tape_.steered) + " steered" : ""),
                                               std::to_string(tape_.history) + " msgs" +
                                                   (tape_.steered ? " · " + std::to_string(tape_.steered) + " steered" : ""),
                                               std::to_string(tape_.history) + " msgs"};
        std::string hist;
        for (const auto& form : hist_forms)
            if (text_width(form) <= vw) { hist = form; break; }
        row(py + 3, "history", hist, val);
        row(py + 4, "tools", std::to_string(tape_.visible) + " in · " + std::to_string(tape_.deferred) + " deferred", val);
        std::string mz = tape_.maze ? "● " : "○ down · ";
        mz += std::to_string(tape_.ledger) + " nodes · " + std::to_string(tape_.edges) + " edges";
        if (tape_.soak_queue) mz += " · " + std::to_string(tape_.soak_queue) + " writing";
        if (tape_.spooled) mz += " · " + std::to_string(tape_.spooled) + " spooled";
        // A refused out publish is the engine not draining the ring: the row is durable in the
        // graph, only this session's ledger view missed it, so the number is shown, never hidden.
        if (tape_.unledgered) mz += " · " + std::to_string(tape_.unledgered) + " unledgered";
        row(py + 5, "maze", mz, tape_.maze ? Style{th_.ok} : Style{th_.error});
    }

    if (th < 3) return;
    // Dynamic height: the tools panel is as tall as its content, never a
    // stretched box. It keeps every call the session has made (entries_ is
    // the rolling history), so it grows as work accumulates instead of
    // padding to a fixed budget with blank rows.
    int want = 0;
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
        if (it->kind == Kind::Tool) ++want;
    if (want == 0) want = std::max<int>(1, static_cast<int>(info_.sources.size()) + 1);
    const int th2 = std::min(th, want + 2);
    const int ty = y + sh + ph + eh + tph;
    f.box(x, ty, w, th2, b, true);
    f.title(x + 2, ty, "tools · " + std::to_string(info_.tools), head, b);
    int ry = ty + 1;
    for (auto it = entries_.rbegin(); it != entries_.rend() && ry < ty + th2 - 1; ++it) {
        if (it->kind != Kind::Tool) continue;
        char32_t g = U'·';
        Style gs = mut;
        if (it->run == Run::Running) { g = kSpin[tick_ % 10]; gs = Style{th_.hot}; }
        else if (it->run == Run::Ok) { g = U'✓'; gs = Style{th_.ok}; }
        else if (it->run == Run::Failed) { g = U'✗'; gs = Style{th_.error}; }
        f.put(x + 2, ry, g, gs);
        const std::string ms = it->ms > 0 ? fmt_ms(it->ms) : "";
        f.text(x + 4, ry, clip(it->name, w - 7 - text_width(ms)), val);
        f.text(x + w - 2 - text_width(ms), ry, ms, mut);
        ++ry;
    }
    if (ry == ty + 1) {
        for (const auto& sc : info_.sources) {
            if (ry >= ty + th2 - 1) break;
            f.put(x + 2, ry, U'◇', Style{th_.accent});
            f.text(x + 4, ry++, clip(sc, w - 6), val);
        }
        if (ry < ty + th2 - 1) f.text(x + 2, ry, "no calls yet", mut);
    }

    // Recent turns, only when the column has leftover rows to spend: one row
    // per turn, newest last. The panel exists when it can say something, so
    // the sidebar never stretches into blank rows.
    const int left = th - th2;
    if (left >= 3 && turns_ > 0) {
        std::vector<std::string> hist;
        for (const auto& e : entries_)
            if (e.kind == Kind::Rule) hist.push_back(e.text);
        const int cap = left - 2;
        if (static_cast<int>(hist.size()) > cap)
            hist.erase(hist.begin(), hist.end() - static_cast<size_t>(cap));
        if (!hist.empty() && cap > 0) {
            const int py2 = ty + th2;
            f.box(x, py2, w, 2 + static_cast<int>(hist.size()), b, true);
            f.title(x + 2, py2, "turns", head, b);
            int ry2 = py2 + 1;
            for (const auto& h : hist) f.text(x + 2, ry2++, clip(h, w - 4), mut);
        }
    }
}

void Scene::paint_prompt(Frame& f, int x, int y, int w, int& cur_x) const {
    const Style b{busy_ ? th_.dim : th_.input_rule};
    f.box(x, y, w, 3, b, true);
    f.title(x + 2, y, "you", Style{th_.gold, kNone, kBold}, b);
    if (busy_) f.title_right(x + w - 2, y, "esc interrupts", Style{th_.warn}, b);
    if (!hint_.empty()) f.title_right(x + w - 2, y + 2, clip(hint_, w - 8), Style{th_.accent}, b);
    else f.title_right(x + w - 2, y + 2, "enter send · ↑↓ history · pgup/pgdn scroll · /help", Style{th_.muted}, b);

    const int sx = x + 2;
    const int sym = f.text(sx, y + 1, th_.prompt_symbol, Style{th_.hot, kNone, kBold});
    const int vx = sx + sym, vw = x + w - 2 - vx;
    cur_x = vx;
    if (input_.empty()) {
        f.text(vx, y + 1, busy_ ? "type ahead — it lands at the next tool round" : "ask the maze…",
               Style{th_.muted, kNone, kItalic}, vw);
        return;
    }
    auto wid = [](char32_t c) { return c == U'\n' ? 1 : cp_width(c); };
    size_t start = 0;   // scroll so the cursor stays inside the row
    int before = 0;
    for (size_t i = 0; i < cursor_; ++i) before += wid(input_[i]);
    while (start < cursor_ && before > vw - 1) before -= wid(input_[start++]);
    int cx = vx;
    for (size_t i = start; i < input_.size(); ++i) {
        const char32_t c = input_[i];
        if (cx + wid(c) > vx + vw) break;
        if (c == U'\n') f.put(cx, y + 1, U'↵', Style{th_.muted});
        else f.put(cx, y + 1, c, Style{th_.prompt});
        if (i < cursor_) cur_x = cx + wid(c);
        cx += wid(c);
    }
    if (start > 0) f.put(vx, y + 1, U'…', Style{th_.muted});
}

void Scene::paint_status(Frame& f, int y, int w) const {
    const uint32_t bg = th_.status_bg;
    f.fill(0, y, w, 1, Style{th_.text, bg});
    std::string left;
    Style ls{th_.ok, bg, kBold};
    if (busy_ && phase_ == Phase::Tools) {
        left = "⚙ " + running_tool_;
        ls = Style{th_.gold, bg, kBold};
    } else if (busy_ && phase_ == Phase::Recalling) {
        const long secs = static_cast<long>(std::chrono::duration<double>(clock::now() - phase_t0_).count());
        left = "◈ recalling from mazemaker · " + std::to_string(secs) + "s";
        ls = Style{th_.accent, bg, kItalic};
    } else if (busy_ && phase_ == Phase::Reasoning) {
        const auto& wing = th_.wings[static_cast<size_t>(tick_ / 4) % th_.wings.size()];
        left = wing.first + " reasoning · ≈" + fmt_tok(live_reason_ ? live_reason_->n : 0) + " tok " + wing.second;
        ls = Style{th_.accent, bg, kItalic};
    } else if (busy_ && phase_ == Phase::Writing) {
        left = "✎ answering · " + fmt_tok(turn_out_ + (live_say_ ? live_say_->n : 0)) + " tok";
        ls = Style{th_.hot, bg, kBold};
    } else if (busy_) {
        const long secs = static_cast<long>(std::chrono::duration<double>(clock::now() - phase_t0_).count());
        const auto& wing = th_.wings[static_cast<size_t>(tick_ / 4) % th_.wings.size()];
        const std::string& verb = th_.verbs[static_cast<size_t>(turns_ * 7 + secs / 4) % th_.verbs.size()];
        left = wing.first + " " + verb + " " + wing.second;
        ls = Style{th_.title, bg, kItalic};
    } else {
        left = "● ready";
    }

    struct Seg { std::string label, value; };
    std::vector<Seg> segs = {{"work ", fmt_ms(work_ms())},
                             {"turn ", std::to_string(turn_calls_) + "/" + std::to_string(info_.max_turns)},
                             {"effort ", info_.effort}};
    if (last_.gen_tps > 0) segs.push_back({"", fmt1(last_.gen_tps) + " t/s"});
    if (info_.n_ctx > 0 && last_.prompt_tokens > 0)
        segs.push_back({"ctx ", std::to_string(static_cast<int>(100.0 * static_cast<double>(last_.prompt_tokens) /
                                                                 static_cast<double>(info_.n_ctx) + 0.5)) + "%"});
    if (tape_.on && tape_.prompt > 0)
        segs.push_back({"reuse ", std::to_string(static_cast<int>(100.0 * static_cast<double>(tape_.reused) /
                                                                   static_cast<double>(tape_.prompt) + 0.5)) + "%"});
    segs.push_back({"", std::to_string(info_.tools) + " tools"});
    if (verb_ == Verbosity::Verbose) {   // accounting only verbose asks for; a narrow row drops these first
        segs.push_back({"calls ", std::to_string(calls_total_)});
        // The turn's running output, not the newest call's: a turn is several calls, and a row
        // that shows only the last one resets the number every call.
        const long turn_out = turn_out_ + (live_say_ ? live_say_->n : 0) + (live_reason_ ? live_reason_->n : 0);
        if (turn_out > 0) segs.push_back({"out ", fmt_tok(turn_out)});
        if (last_.cached_tokens > 0) segs.push_back({"cache ", fmt_tok(last_.cached_tokens)});
        if (last_.attempts > 1) segs.push_back({"sent ", std::to_string(last_.attempts)});
        if (tape_.on) segs.push_back({"epoch ", std::to_string(tape_.epoch) + " · " + fmt_tok(tape_.tokens)});
        if (tape_.on && tape_.deferred > 0) segs.push_back({"deferred ", std::to_string(tape_.deferred)});
        if (plan_.total > 0) segs.push_back({"plan ", std::to_string(plan_.open) + "/" + std::to_string(plan_.total)});
        segs.push_back({"mode ", "verbose"});
    }
    const std::string brand = "⟁ " + th_.agent_name;   // the skin's name, not the repo's folder

    auto width_of = [&](size_t n, bool with_brand) {
        int t = 1 + text_width(left);
        for (size_t i = 0; i < n; ++i) t += 3 + text_width(segs[i].label + segs[i].value);
        return t + (with_brand ? 2 + text_width(brand) : 0) + 1;
    };
    bool show_brand = true;
    size_t n = segs.size();
    while (width_of(n, show_brand) > w && (show_brand || n > 0)) {
        if (show_brand) show_brand = false;
        else --n;
    }
    int cx = 1 + f.text(1, y, clip(left, w - 2), ls);
    for (size_t i = 0; i < n; ++i) {
        cx += f.text(cx, y, " │ ", Style{th_.dim, bg});
        cx += f.text(cx, y, segs[i].label, Style{th_.muted, bg});
        cx += f.text(cx, y, segs[i].value, Style{th_.text, bg, kBold});
    }
    if (show_brand) f.text(w - 1 - text_width(brand), y, brand, Style{th_.muted, bg});
}

std::string Scene::present(int cols, int rows, bool truecolor) {
    apply();
    ++tick_;
    if (!frame_ || frame_->w() != cols || frame_->h() != rows) frame_ = std::make_unique<Frame>(cols, rows);
    Frame& f = *frame_;
    f.fill(0, 0, cols, rows, Style{});
    std::string tail = "\x1b[?25l";
    if (cols < kMinCols || rows < kMinRows) {
        const std::string msg = "⟁ " + std::to_string(cols) + "×" + std::to_string(rows) + " — dd_tui needs " +
                                std::to_string(kMinCols) + "×" + std::to_string(kMinRows);
        f.text(std::max(0, (cols - text_width(msg)) / 2), rows / 2, clip(msg, cols), Style{th_.warn});
    } else {
        const int body_h = rows - 4;
        const int side = cols >= 100 ? kSidebarW : 0;
        paint_transcript(f, 0, 0, cols - side, body_h);
        if (side) paint_sidebar(f, cols - side, 0, side, body_h);
        int cur_x = 0;
        paint_prompt(f, 0, body_h, cols, cur_x);
        paint_status(f, rows - 1, cols);
        tail = "\x1b[" + std::to_string(body_h + 2) + ";" + std::to_string(cur_x + 1) + "H\x1b[?25h";
    }
    std::string body = f.present(truecolor);
    if (body.empty() && tail == last_tail_) return {};
    last_tail_ = tail;
    return "\x1b[?2026h\x1b[?25l" + body + tail + "\x1b[?2026l";
}

std::string Scene::drain_inline(int width, bool color, bool truecolor) {
    apply();
    const int w = std::clamp(width, 40, kCardMaxW);
    std::string out;
    while (printed_ < entries_.size()) {
        const Entry& e = entries_[printed_];
        if ((e.kind == Kind::Tool || e.kind == Kind::Say || e.kind == Kind::Reason) && !e.resolved) break;   // cards print whole
        out += render(e, w).to_ansi(color, truecolor);
        ++printed_;
    }
    return out;
}

size_t Scene::live_text_bytes() const {
    return (live_reason_ ? live_reason_->text.size() : 0) + (live_say_ ? live_say_->text.size() : 0);
}

// What the rings refused. Both are numbers the reply names, so an over-full ring is a fact the
// operator sees rather than text that vanished.
uint64_t Scene::dropped_events() const { return q_.refused(); }
uint64_t Scene::dropped_live_bytes() const { return dropped_live_.load(std::memory_order_relaxed); }

std::string Scene::row_text(int y) const {
    return frame_ ? frame_->row_text(y) : std::string();
}

}  // namespace ui
}  // namespace dd
