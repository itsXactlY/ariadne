// Faithful port of tools/fuzzy_match.py — 8-strategy fuzzy find-and-replace
// plus an inline reimplementation of difflib.SequenceMatcher.ratio() (isjunk=None,
// autojunk=True). Public entry point: fuzzy_find_and_replace.
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "dd/tools/fuzzy_match.hpp"

namespace dd {
namespace tools {

// ---------- small string helpers (mirror Python str.split/strip/lstrip) ----------
static std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

static std::string strip(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && is_ws(s[a])) ++a;
    while (b > a && is_ws(s[b - 1])) --b;
    return s.substr(a, b - a);
}

static std::string lstrip(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && is_ws(s[a])) ++a;
    return s.substr(a, b - a);
}

static std::string join_range(const std::vector<std::string>& v, int a, int b) {
    std::string out;
    for (int i = a; i < b; ++i) {
        if (i > a) out.push_back('\n');
        out += v[i];
    }
    return out;
}

static std::string join_all(const std::vector<std::string>& v) { return join_range(v, 0, (int)v.size()); }

static std::string replace_all_bytes(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    std::string out;
    std::size_t i = 0;
    while (true) {
        std::size_t p = s.find(from, i);
        if (p == std::string::npos) {
            out += s.substr(i);
            break;
        }
        out += s.substr(i, p - i);
        out += to;
        i = p + from.size();
    }
    return out;
}

// Strategy 3 normalize: re.sub(r'[ \t]+', ' ', s)
static std::string normalize_ws(const std::string& s) {
    std::string out;
    bool in_ws = false;
    for (char c : s) {
        if (c == ' ' || c == '\t') {
            if (!in_ws) {
                out.push_back(' ');
                in_ws = true;
            }
        } else {
            out.push_back(c);
            in_ws = false;
        }
    }
    return out;
}

// Strategy 5 unescape: replace \\n -> \n, \\t -> \t, \\r -> \r (in order)
static std::string unescape(const std::string& s) {
    std::string t = replace_all_bytes(s, "\\n", "\n");
    t = replace_all_bytes(t, "\\t", "\t");
    return replace_all_bytes(t, "\\r", "\r");
}

// _unicode_normalize: apply UNICODE_MAP in Python dict insertion order (full codepoint keys as UTF-8).
static std::string unicode_normalize(const std::string& s) {
    std::string out = s;
    out = replace_all_bytes(out, "\xE2\x80\x9C", "\"");  // U+201c
    out = replace_all_bytes(out, "\xE2\x80\x9D", "\"");  // U+201d
    out = replace_all_bytes(out, "\xE2\x80\x98", "'");   // U+2018
    out = replace_all_bytes(out, "\xE2\x80\x99", "'");   // U+2019
    out = replace_all_bytes(out, "\xE2\x80\x94", "--");  // U+2014
    out = replace_all_bytes(out, "\xE2\x80\x93", "-");   // U+2013
    out = replace_all_bytes(out, "\xE2\x80\xA6", "..."); // U+2026
    out = replace_all_bytes(out, "\xC2\xA0", " ");       // U+00a0
    return out;
}

// ---------- difflib.SequenceMatcher.ratio() (codepoint-aware) ----------
static std::vector<std::uint32_t> decode_utf8(const std::string& s) {
    std::vector<std::uint32_t> out;
    std::size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        std::uint32_t cp;
        int len;
        if (c < 0x80) {
            cp = c;
            len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            len = 4;
        } else {
            cp = c;
            len = 1;
        }
        for (int t = 1; t < len && i + t < n; ++t) cp = (cp << 6) | ((unsigned char)s[i + t] & 0x3F);
        out.push_back(cp);
        i += len;
    }
    return out;
}

// SequenceMatcher(None, a, b).ratio(). isjunk=None, autojunk=True.
static double seq_ratio(const std::string& a, const std::string& b) {
    std::vector<std::uint32_t> A = decode_utf8(a);
    std::vector<std::uint32_t> B = decode_utf8(b);
    const int la = (int)A.size();
    const int lb = (int)B.size();

    // b2j: char -> sorted index list in B (chained, isbjunk empty).
    std::map<std::uint32_t, std::vector<int>> b2j;
    for (int idx = 0; idx < lb; ++idx) b2j[B[idx]].push_back(idx);

    // autojunk: when len(b) >= 200, drop popular chars (count > lb/100 + 1).
    if (lb >= 200) {
        int ntest = lb / 100 + 1;
        std::vector<std::uint32_t> popular;
        for (auto& kv : b2j) {
            if ((int)kv.second.size() > ntest) popular.push_back(kv.first);
        }
        for (std::uint32_t e : popular) b2j.erase(e);
    }

    // find_longest_match (bjunk empty => isbjunk always false).
    auto find_longest = [&](int alo, int ahi, int blo, int bhi, int& besti, int& bestj, int& bestsize) {
        besti = alo;
        bestj = blo;
        bestsize = 0;
        std::map<int, int> j2len;
        for (int i = alo; i < ahi; ++i) {
            std::map<int, int> newj2len;
            auto it = b2j.find(A[i]);
            if (it != b2j.end()) {
                for (int j : it->second) {
                    if (j < blo) continue;
                    if (j >= bhi) break;
                    int prev = 0;
                    auto pj = j2len.find(j - 1);
                    if (pj != j2len.end()) prev = pj->second;
                    int k = prev + 1;
                    newj2len[j] = k;
                    if (k > bestsize) {
                        besti = i - k + 1;
                        bestj = j - k + 1;
                        bestsize = k;
                    }
                }
            }
            j2len = std::move(newj2len);
        }
        // extend with non-junk characters (bjunk empty)
        while (besti > alo && bestj > blo && A[besti - 1] == B[bestj - 1]) {
            --besti;
            --bestj;
            ++bestsize;
        }
        while (besti + bestsize < ahi && bestj + bestsize < bhi && A[besti + bestsize] == B[bestj + bestsize]) {
            ++bestsize;
        }
    };

    // get_matching_blocks (recursive queue; sum of block sizes == matches)
    long long matches = 0;
    std::vector<std::array<int, 4>> queue;
    queue.push_back({0, la, 0, lb});
    while (!queue.empty()) {
        std::array<int, 4> q = queue.back();
        queue.pop_back();
        int alo = q[0], ahi = q[1], blo = q[2], bhi = q[3];
        int i, j, k;
        find_longest(alo, ahi, blo, bhi, i, j, k);
        if (k) {
            matches += k;
            if (alo < i && blo < j) queue.push_back({alo, i, blo, j});
            if (i + k < ahi && j + k < bhi) queue.push_back({i + k, ahi, j + k, bhi});
        }
    }
    long long total = (long long)la + (long long)lb;
    if (total) return 2.0 * (double)matches / (double)total;
    return 1.0;
}

// ---------- position helpers ----------
static std::pair<int, int> calculate_line_positions(const std::vector<std::string>& content_lines,
                                                    int start_line, int end_line, int content_length) {
    int start_pos = 0;
    for (int t = 0; t < start_line; ++t) start_pos += (int)content_lines[t].size() + 1;
    int end_pos = 0;
    for (int t = 0; t < end_line; ++t) end_pos += (int)content_lines[t].size() + 1;
    end_pos -= 1;
    if (end_pos >= content_length) end_pos = content_length;
    return {start_pos, end_pos};
}

static std::vector<std::pair<int, int>> find_normalized_matches(const std::string& content,
                                                                const std::vector<std::string>& content_lines,
                                                                const std::vector<std::string>& content_norm_lines,
                                                                const std::string& pattern,
                                                                const std::string& pattern_normalized) {
    (void)pattern;
    auto pattern_norm_lines = split_lines(pattern_normalized);
    const int num = (int)pattern_norm_lines.size();
    std::vector<std::pair<int, int>> matches;
    for (int i = 0; i <= (int)content_norm_lines.size() - num; ++i) {
        std::string block = join_range(content_norm_lines, i, i + num);
        if (block == pattern_normalized) {
            auto pe = calculate_line_positions(content_lines, i, i + num, (int)content.size());
            matches.push_back(pe);
        }
    }
    return matches;
}

// Strategy 3 helper: map normalized positions back to original.
static std::vector<std::pair<int, int>> map_normalized_positions(const std::string& original,
                                                                 const std::string& normalized,
                                                                 const std::vector<std::pair<int, int>>& normalized_matches) {
    if (normalized_matches.empty()) return {};
    std::vector<int> orig_to_norm;
    int orig_idx = 0, norm_idx = 0;
    const int olen = (int)original.size(), nlen = (int)normalized.size();
    while (orig_idx < olen && norm_idx < nlen) {
        if (original[orig_idx] == normalized[norm_idx]) {
            orig_to_norm.push_back(norm_idx);
            ++orig_idx;
            ++norm_idx;
        } else if ((original[orig_idx] == ' ' || original[orig_idx] == '\t') && normalized[norm_idx] == ' ') {
            orig_to_norm.push_back(norm_idx);
            ++orig_idx;
            if (orig_idx < olen && !(original[orig_idx] == ' ' || original[orig_idx] == '\t')) ++norm_idx;
        } else if (original[orig_idx] == ' ' || original[orig_idx] == '\t') {
            orig_to_norm.push_back(norm_idx);
            ++orig_idx;
        } else {
            orig_to_norm.push_back(norm_idx);
            ++orig_idx;
        }
    }
    while (orig_idx < olen) {
        orig_to_norm.push_back(nlen);
        ++orig_idx;
    }

    std::map<int, int> norm_to_orig_start, norm_to_orig_end;
    for (int orig_pos = 0; orig_pos < (int)orig_to_norm.size(); ++orig_pos) {
        int np = orig_to_norm[orig_pos];
        if (!norm_to_orig_start.count(np)) norm_to_orig_start[np] = orig_pos;
        norm_to_orig_end[np] = orig_pos;
    }

    std::vector<std::pair<int, int>> out;
    for (auto& mm : normalized_matches) {
        int norm_start = mm.first, norm_end = mm.second;
        int orig_start;
        auto sit = norm_to_orig_start.find(norm_start);
        if (sit != norm_to_orig_start.end()) {
            orig_start = sit->second;
        } else {
            int best = (int)orig_to_norm.size();
            for (int i = 0; i < (int)orig_to_norm.size(); ++i) {
                if (orig_to_norm[i] >= norm_start) {
                    best = i;
                    break;
                }
            }
            orig_start = best;
        }
        int orig_end;
        auto eit = norm_to_orig_end.find(norm_end - 1);
        if (eit != norm_to_orig_end.end()) {
            orig_end = eit->second + 1;
        } else {
            orig_end = orig_start + (norm_end - norm_start);
        }
        while (orig_end < olen && (original[orig_end] == ' ' || original[orig_end] == '\t')) ++orig_end;
        out.push_back({orig_start, std::min(orig_end, olen)});
    }
    return out;
}

static std::string apply_replacements(const std::string& content, std::vector<std::pair<int, int>> matches,
                                      const std::string& new_string) {
    std::stable_sort(matches.begin(), matches.end(),
                     [](const std::pair<int, int>& x, const std::pair<int, int>& y) { return x.first > y.first; });
    std::string result = content;
    for (auto& pe : matches) {
        result = result.substr(0, pe.first) + new_string + result.substr(pe.second);
    }
    return result;
}

// ---------- the eight strategies ----------
static std::vector<std::pair<int, int>> strategy_exact(const std::string& content, const std::string& pattern) {
    std::vector<std::pair<int, int>> m;
    if (pattern.empty()) return m;
    std::size_t start = 0;
    while (true) {
        std::size_t pos = content.find(pattern, start);
        if (pos == std::string::npos) break;
        m.push_back({(int)pos, (int)(pos + pattern.size())});
        start = pos + 1;
    }
    return m;
}

static std::vector<std::pair<int, int>> strategy_line_trimmed(const std::string& content, const std::string& pattern) {
    auto pl = split_lines(pattern);
    for (auto& l : pl) l = strip(l);
    std::string pnorm = join_all(pl);
    auto cl = split_lines(content);
    auto cnl = cl;
    for (auto& l : cnl) l = strip(l);
    return find_normalized_matches(content, cl, cnl, pattern, pnorm);
}

static std::vector<std::pair<int, int>> strategy_whitespace_normalized(const std::string& content,
                                                                       const std::string& pattern) {
    std::string pnorm = normalize_ws(pattern);
    std::string cnorm = normalize_ws(content);
    auto m = strategy_exact(cnorm, pnorm);
    if (m.empty()) return m;
    return map_normalized_positions(content, cnorm, m);
}

static std::vector<std::pair<int, int>> strategy_indentation_flexible(const std::string& content,
                                                                      const std::string& pattern) {
    auto cl = split_lines(content);
    auto csl = cl;
    for (auto& l : csl) l = lstrip(l);
    auto pl = split_lines(pattern);
    for (auto& l : pl) l = lstrip(l);
    std::string pnorm = join_all(pl);
    return find_normalized_matches(content, cl, csl, pattern, pnorm);
}

static std::vector<std::pair<int, int>> strategy_escape_normalized(const std::string& content,
                                                                   const std::string& pattern) {
    std::string pu = unescape(pattern);
    if (pu == pattern) return {};
    return strategy_exact(content, pu);
}

static std::vector<std::pair<int, int>> strategy_trimmed_boundary(const std::string& content,
                                                                  const std::string& pattern) {
    auto pl = split_lines(pattern);
    if (pl.empty()) return {};
    pl[0] = strip(pl[0]);
    if (pl.size() > 1) pl[pl.size() - 1] = strip(pl[pl.size() - 1]);
    std::string mod = join_all(pl);
    auto cl = split_lines(content);
    const int count = (int)pl.size();
    std::vector<std::pair<int, int>> matches;
    for (int i = 0; i <= (int)cl.size() - count; ++i) {
        std::vector<std::string> check(cl.begin() + i, cl.begin() + i + count);
        check[0] = strip(check[0]);
        if (check.size() > 1) check[check.size() - 1] = strip(check[check.size() - 1]);
        if (join_all(check) == mod) {
            matches.push_back(calculate_line_positions(cl, i, i + count, (int)content.size()));
        }
    }
    return matches;
}

static std::vector<std::pair<int, int>> strategy_block_anchor(const std::string& content, const std::string& pattern) {
    std::string npat = unicode_normalize(pattern);
    std::string ncon = unicode_normalize(content);
    auto pl = split_lines(npat);
    if (pl.size() < 2) return {};
    std::string first = strip(pl[0]);
    std::string last = strip(pl[pl.size() - 1]);
    auto ncl = split_lines(ncon);
    auto ocl = split_lines(content);
    const int count = (int)pl.size();
    std::vector<int> potential;
    for (int i = 0; i <= (int)ncl.size() - count; ++i) {
        if (strip(ncl[i]) == first && strip(ncl[i + count - 1]) == last) potential.push_back(i);
    }
    std::vector<std::pair<int, int>> matches;
    double threshold = ((int)potential.size() == 1) ? 0.10 : 0.30;
    for (int i : potential) {
        double sim;
        if (count <= 2) {
            sim = 1.0;
        } else {
            std::string cmid = join_range(ncl, i + 1, i + count - 1);
            std::string pmid = join_range(pl, 1, count - 1);
            sim = seq_ratio(cmid, pmid);
        }
        if (sim >= threshold) matches.push_back(calculate_line_positions(ocl, i, i + count, (int)content.size()));
    }
    return matches;
}

static std::vector<std::pair<int, int>> strategy_context_aware(const std::string& content, const std::string& pattern) {
    auto pl = split_lines(pattern);
    auto cl = split_lines(content);
    if (pl.empty()) return {};
    const int count = (int)pl.size();
    std::vector<std::pair<int, int>> matches;
    for (int i = 0; i <= (int)cl.size() - count; ++i) {
        int high = 0;
        for (int t = 0; t < count; ++t) {
            double sim = seq_ratio(strip(pl[t]), strip(cl[i + t]));
            if (sim >= 0.80) ++high;
        }
        if ((double)high >= (double)count * 0.5)
            matches.push_back(calculate_line_positions(cl, i, i + count, (int)content.size()));
    }
    return matches;
}

// ---------- public entry ----------
static FuzzyResult make_result(const std::string& content, int mc, std::optional<std::string> err) {
    FuzzyResult r;
    r.content = content;
    r.match_count = mc;
    r.error = err;
    return r;
}

FuzzyResult fuzzy_find_and_replace(const std::string& content, const std::string& old_string,
                                   const std::string& new_string, bool replace_all) {
    if (old_string.empty()) return make_result(content, 0, "old_string cannot be empty");
    if (old_string == new_string) return make_result(content, 0, "old_string and new_string are identical");

    auto attempt = [&](std::vector<std::pair<int, int>> m) -> std::optional<FuzzyResult> {
        if (m.empty()) return std::nullopt;
        if ((int)m.size() > 1 && !replace_all) {
            std::string msg = "Found " + std::to_string((int)m.size()) +
                              " matches for old_string. Provide more context to make it unique, or use replace_all=True.";
            return make_result(content, 0, msg);
        }
        std::string nc = apply_replacements(content, m, new_string);
        return make_result(nc, (int)m.size(), std::nullopt);
    };

    if (auto r = attempt(strategy_exact(content, old_string))) return *r;
    if (auto r = attempt(strategy_line_trimmed(content, old_string))) return *r;
    if (auto r = attempt(strategy_whitespace_normalized(content, old_string))) return *r;
    if (auto r = attempt(strategy_indentation_flexible(content, old_string))) return *r;
    if (auto r = attempt(strategy_escape_normalized(content, old_string))) return *r;
    if (auto r = attempt(strategy_trimmed_boundary(content, old_string))) return *r;
    if (auto r = attempt(strategy_block_anchor(content, old_string))) return *r;
    if (auto r = attempt(strategy_context_aware(content, old_string))) return *r;

    return make_result(content, 0, "Could not find a match for old_string in the file");
}

}  // tools
}  // dd
