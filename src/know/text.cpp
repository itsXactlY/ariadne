// src/know/text.cpp — impl of dd/know/text.hpp.
#include "dd/know/text.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <unordered_set>

namespace dd {
namespace know {

namespace {

// ---- UTF-8 ----

// Decodes one code point at s[i]; advances i. A broken byte is its own code point.
uint32_t next_cp(std::string_view s, size_t& i) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
        ++i;
        return c;
    }
    int n = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
    uint32_t cp = n == 3 ? (c & 0x07u) : n == 2 ? (c & 0x0Fu) : n == 1 ? (c & 0x1Fu) : c;
    ++i;
    while (n-- > 0 && i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) cp = (cp << 6) | (static_cast<unsigned char>(s[i++]) & 0x3Fu);
    return cp;
}

// The folded spelling of a non-ASCII letter, or nullptr when it is kept as it is.
const char* fold_cp(uint32_t cp) {
    switch (cp) {
        case 0xC4: case 0xE4: return "ae";
        case 0xD6: case 0xF6: return "oe";
        case 0xDC: case 0xFC: return "ue";
        case 0xDF: return "ss";
        case 0xC6: case 0xE6: return "ae";
        case 0x152: case 0x153: return "oe";
        case 0xC7: case 0xE7: return "c";
        case 0xD1: case 0xF1: return "n";
        case 0xD8: case 0xF8: return "o";
        case 0xDD: case 0xFD: case 0xFF: return "y";
        default: break;
    }
    if ((cp >= 0xC0 && cp <= 0xC5) || (cp >= 0xE0 && cp <= 0xE5)) return "a";
    if ((cp >= 0xC8 && cp <= 0xCB) || (cp >= 0xE8 && cp <= 0xEB)) return "e";
    if ((cp >= 0xCC && cp <= 0xCF) || (cp >= 0xEC && cp <= 0xEF)) return "i";
    if ((cp >= 0xD2 && cp <= 0xD5) || (cp >= 0xF2 && cp <= 0xF5)) return "o";
    if ((cp >= 0xD9 && cp <= 0xDB) || (cp >= 0xF9 && cp <= 0xFB)) return "u";
    return nullptr;
}

// Symbols, punctuation, arrows, box drawing, emoji: a word break, never part of a word.
bool symbol_cp(uint32_t cp) {
    if (cp < 0x80) return false;
    if (cp < 0xC0) return true;                          // Latin-1 punctuation and symbols
    if (cp == 0xD7 || cp == 0xF7) return true;           // x /
    if (cp >= 0x2000 && cp < 0x3000) return true;        // punctuation, arrows, math, box drawing, dingbats
    if (cp >= 0x3000 && cp < 0x3040) return true;        // CJK punctuation
    if (cp >= 0xFE00 && cp < 0xFE10) return true;        // variation selectors
    if (cp >= 0x1F000) return true;                      // emoji and pictographs
    return false;
}

// ---- stopwords: English and German, folded spelling ----

const std::unordered_set<std::string_view>& stopwords() {
    static const std::unordered_set<std::string_view> s = {
        // English
        "a", "about", "above", "after", "again", "all", "also", "am", "an", "and", "any", "are", "as", "at", "be",
        "because", "been", "before", "being", "below", "between", "both", "but", "by", "can", "could", "did", "do",
        "does", "doing", "down", "during", "each", "few", "for", "from", "further", "had", "has", "have", "having",
        "he", "her", "here", "hers", "him", "his", "how", "if", "in", "into", "is", "it", "its", "itself", "just",
        "me", "more", "most", "my", "myself", "no", "nor", "not", "now", "of", "off", "on", "once", "only", "or",
        "other", "our", "ours", "out", "over", "own", "same", "she", "should", "so", "some", "such", "than", "that",
        "the", "their", "theirs", "them", "then", "there", "these", "they", "this", "those", "through", "to", "too",
        "under", "until", "up", "very", "was", "we", "were", "what", "when", "where", "which", "while", "who", "whom",
        "why", "will", "with", "would", "you", "your", "yours", "yourself", "ok", "okay", "yes", "yeah", "lol",
        "please", "thanks", "thank", "get", "got", "let", "lets", "make", "want", "need", "like", "really", "still",
        "even", "much", "many", "one", "two", "use", "using", "used", "maybe", "actually", "basically", "already",
        "always", "never", "ever", "another", "every", "everything", "something", "anything", "dont", "doesnt",
        "didnt", "isnt", "arent", "wasnt", "cant", "wont", "im", "ive", "ill", "youre", "thats", "theres", "whats",
        "wtf", "ffs", "omg", "idk", "imo", "btw", "fyi", "tbh", "smh", "ugh", "hmm", "yep", "nope", "gonna", "wanna",
        // German (folded: ä ö ü ß -> ae oe ue ss)
        "aber", "alle", "allem", "allen", "aller", "alles", "als", "also", "am", "an", "ander", "andere", "anderen",
        "auch", "auf", "aus", "bei", "bin", "bis", "bist", "da", "dadurch", "dafuer", "fuer", "dagegen", "daher", "damit",
        "danach", "dann", "das", "dass", "daran", "darauf", "darin", "darum", "davon", "dazu", "dein", "deine",
        "deinen", "deiner", "dem", "den", "denn", "der", "des", "dessen", "deshalb", "die", "dies", "diese", "diesem",
        "diesen", "dieser", "dieses", "doch", "dort", "du", "durch", "ein", "eine", "einem", "einen", "einer",
        "eines", "einmal", "er", "es", "etwas", "euch", "euer", "gegen", "gewesen", "hab", "habe", "haben", "hast",
        "hat", "hatte", "hatten", "hin", "hinter", "ich", "ihm", "ihn", "ihnen", "ihr", "ihre", "ihrem", "ihren",
        "ihrer", "im", "indem", "ins", "ist", "ja", "jede", "jedem", "jeden", "jeder", "jedes", "jetzt", "kann",
        "kannst", "kein", "keine", "keinen", "keiner", "koennen", "koennte", "machen", "mal", "man", "mehr", "mein",
        "meine", "meinen", "meiner", "mich", "mir", "mit", "muss", "musst", "nach", "nein", "nicht", "nichts", "noch",
        "nun", "nur", "ob", "oder", "ohne", "sehr", "sein", "seine", "seinen", "seiner", "sich", "sie", "sind", "so",
        "soll", "sollen", "sollte", "sonst", "ueber", "um", "und", "uns", "unser", "unsere", "unter", "vom", "von",
        "vor", "waehrend", "war", "waren", "warum", "was", "wegen", "weil", "weiter", "welche", "wenn", "wer",
        "werde", "werden", "wie", "wieder", "will", "wir", "wird", "wirst", "wo", "woher", "wohin", "wollen",
        "wurde", "wurden", "zu", "zum", "zur", "zwischen", "halt", "eben", "schon", "gerade", "bitte", "danke",
        "ne", "nee", "jo", "ach", "oh", "hm", "irgendwie", "irgend", "ganz", "gar", "bloss", "kaum"};
    return s;
}

const std::unordered_set<std::string_view>& generic_path_words() {
    static const std::unordered_set<std::string_view> s = {
        "home", "usr", "var", "etc", "tmp", "opt", "bin", "sbin", "lib", "lib64", "share", "local", "src", "source",
        "sources", "include", "includes", "build", "builds", "dist", "target", "node_modules", "docs", "doc", "test",
        "tests", "examples", "example", "projects", "project", "repos", "repo", "code", "dev", "work", "workspace",
        "mnt", "media", "root", "proc", "sys", "run", "boot", "snap", "cache", "config", "data", "output", "out",
        "release", "debug", "main", "master", "pkg", "vendor", "third_party", "scripts", "script", "tools", "tool"};
    return s;
}

bool is_digit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }
bool is_upper(char c) { return std::isupper(static_cast<unsigned char>(c)) != 0; }
bool is_lower(char c) { return std::islower(static_cast<unsigned char>(c)) != 0; }
bool is_alnum(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }

// "0.1.45", "v1.2", "3.14.7-rc1"
bool version_shaped(const std::string& w) {
    size_t i = (w.size() > 1 && (w[0] == 'v' || w[0] == 'V') && is_digit(w[1])) ? 1 : 0;
    int dots = 0;
    bool digit_seen = false;
    for (; i < w.size(); ++i) {
        if (is_digit(w[i])) digit_seen = true;
        else if (w[i] == '.' && digit_seen) ++dots;
        else if ((w[i] == '-' || w[i] == '+') && dots >= 1) return true;
        else return false;
    }
    return dots >= 1 && digit_seen && w.back() != '.';
}

// ENOENT, SIGSEGV, ERR_CONNECTION, E0433
bool error_shaped(const std::string& w) {
    if (w.size() < 4) return false;
    if (w[0] == 'E' && std::all_of(w.begin() + 1, w.end(), is_digit) && w.size() <= 6) return true;
    int upper = 0;
    for (char c : w) {
        if (is_upper(c)) ++upper;
        else if (!(is_digit(c) || c == '_')) return false;
    }
    return upper >= 4;
}

std::string lower_ascii(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Plural, -ing, -ed and a final e fold together (containers/container, deployed/deploy, caches/cache).
// Only plain lowercase words; both sides of a match go through the same function, so it need not be right, only the same.
std::string stem(std::string w) {
    if (w.size() < 4 || !std::all_of(w.begin(), w.end(), [](char c) { return c >= 'a' && c <= 'z'; })) return w;
    auto ends = [&](const char* suf) {
        const std::string t(suf);
        return w.size() >= t.size() && w.compare(w.size() - t.size(), t.size(), t) == 0;
    };
    auto undouble = [&] {
        const char c = w.back();
        if (w.size() >= 4 && w[w.size() - 2] == c && std::string_view("aeiouylsz").find(c) == std::string_view::npos) w.pop_back();
    };
    if (w.size() >= 5 && ends("ies")) {
        w.resize(w.size() - 3);
        w += "y";
    } else if (!ends("ss") && !ends("us") && !ends("is") && w.back() == 's') {
        w.pop_back();
    }
    if (w.size() >= 7 && ends("ing")) {
        w.resize(w.size() - 3);
        undouble();
    } else if (w.size() >= 6 && ends("ed")) {
        w.resize(w.size() - 2);
        undouble();
    }
    if (w.size() >= 5 && w.back() == 'e') w.pop_back();
    return w;
}

}  // namespace

bool is_stopword(std::string_view folded) { return stopwords().count(folded) > 0; }
bool is_generic_path_word(std::string_view lower) { return generic_path_words().count(lower) > 0; }

std::vector<std::string> terms_of(std::string_view text, bool keep_stop) {
    std::vector<std::string> out;
    std::string cur, run;   // cur: the part (camelCase split); run: the whole alphanumeric run it sits in
    bool split_in_run = false;
    auto emit = [&](const std::string& tok) {
        const bool digits = std::all_of(tok.begin(), tok.end(), is_digit);
        const bool keep = digits ? tok.size() >= 3 : tok.size() >= 2;
        if (keep && (keep_stop || !is_stopword(tok))) out.push_back(stem(tok));
    };
    auto flush = [&] {
        if (!cur.empty()) emit(cur);
        cur.clear();
    };
    auto end_run = [&] {
        flush();
        if (split_in_run && run.size() >= 3) emit(run);   // BTQuant is also btquant
        run.clear();
        split_in_run = false;
    };

    // a version or an error code is also a term as written
    {
        size_t i = 0;
        while (i < text.size()) {
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r')) ++i;
            size_t j = i;
            while (j < text.size() && text[j] != ' ' && text[j] != '\t' && text[j] != '\n' && text[j] != '\r') ++j;
            if (j > i) {
                std::string w(text.substr(i, j - i));
                while (!w.empty() && std::string_view("()[]{}<>,;:\"'`").find(w.front()) != std::string_view::npos) w.erase(w.begin());
                while (!w.empty() && (std::string_view("()[]{}<>,;:\"'`.!?").find(w.back()) != std::string_view::npos)) w.pop_back();
                if (version_shaped(w) || error_shaped(w)) out.push_back(lower_ascii(w));
            }
            i = j;
        }
    }

    size_t i = 0;
    char prev = 0;   // the last letter's case: 'A' upper, 'a' lower or other, 0 at a break
    while (i < text.size()) {
        const size_t at = i;
        const uint32_t cp = next_cp(text, i);
        if (cp < 0x80) {
            const char c = static_cast<char>(cp);
            if (!is_alnum(c)) {
                end_run();
                prev = 0;
                continue;
            }
            // camelCase: readFile -> read file, HTTPServer -> http server
            if (is_upper(c) && !cur.empty()) {
                const bool next_lower = i < text.size() && is_lower(text[i]);
                if (prev == 'a' || (prev == 'A' && next_lower)) {
                    flush();
                    split_in_run = true;
                }
            }
            const char l = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            cur.push_back(l);
            run.push_back(l);
            prev = is_upper(c) ? 'A' : 'a';
        } else if (symbol_cp(cp)) {
            end_run();
            prev = 0;
        } else {
            const char* f = fold_cp(cp);
            const bool upper = cp >= 0xC0 && cp <= 0xDE;   // Ä Ö Ü É ...: the case of what follows is judged from it
            if (f) {
                cur += f;
                run += f;
            } else {
                cur.append(text.substr(at, i - at));   // another script: as it is
                run.append(text.substr(at, i - at));
            }
            prev = upper ? 'A' : 'a';
        }
    }
    end_run();
    return out;
}

// ---- Distiller ----

namespace {

bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// A line of terminal output, a prompt, a log or a table: pasted, not written.
bool terminal_line(const std::string& line) {
    const size_t b = line.find_first_not_of(" \t");
    if (b == std::string::npos) return false;
    const std::string t = line.substr(b);
    static const char* glyphs[] = {"\xE2\x95", "\xE2\x94", "\xE2\x9D\xAF", "\xCE\xBB ", "$ ", "> ", "\xE2\x9C", "\xE2\x9E", "\xE2\x87"};
    for (const char* g : glyphs)
        if (starts_with(t, g)) return true;
    if (starts_with(t, "[20") || (starts_with(t, "20") && t.size() > 10 && t[4] == '-' && t[7] == '-')) return true;
    if (starts_with(t, "{\"") || starts_with(t, "HTTP/") || (starts_with(t, "at ") && t.find('(') != std::string::npos)) return true;
    size_t alpha = 0, space = 0, other = 0;
    for (char c : t) {
        if (is_alnum(c) || (static_cast<unsigned char>(c) >= 0x80)) ++alpha;
        else if (c == ' ') ++space;
        else ++other;
    }
    return alpha + space > 0 && static_cast<double>(other) / static_cast<double>(alpha + space + other) > 0.30;
}

std::string drop_between(std::string s, const std::string& open, const std::string& close) {
    for (size_t b; (b = s.find(open)) != std::string::npos;) {
        size_t e = s.find(close, b + open.size());
        if (e == std::string::npos) e = s.size();
        else e += close.size();
        s.erase(b, e - b);
    }
    return s;
}

std::string squash(const std::string& s) {
    std::string o;
    bool sp = false;
    for (char c : s) {
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
            sp = !o.empty();
            continue;
        }
        if (sp) o.push_back(' ');
        sp = false;
        o.push_back(c);
    }
    return o;
}

std::string utf8_head(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return s.substr(0, n);
}
std::string utf8_tail(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    size_t from = s.size() - n;
    while (from < s.size() && (static_cast<unsigned char>(s[from]) & 0xC0) == 0x80) ++from;
    return s.substr(from);
}

// "http://x/y" -> ""
std::string without_urls(const std::string& s) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        if (s.compare(i, 7, "http://") == 0 || s.compare(i, 8, "https://") == 0) {
            while (i < s.size() && s[i] != ' ' && s[i] != '\n' && s[i] != '\t') ++i;
            continue;
        }
        out.push_back(s[i++]);
    }
    return out;
}

const std::string& user_name() {
    static const std::string u = [] {
        const char* v = std::getenv("USER");
        return v ? lower_ascii(v) : std::string();
    }();
    return u;
}

// The words a path is about: its project and file, never the generic folders on the way.
void path_terms(const std::string& word, std::vector<Term>& out) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : word) {
        if (c == '/') {
            if (!cur.empty()) parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    for (size_t i = 0; i < parts.size(); ++i) {
        const bool last = i + 1 == parts.size();
        std::string p = parts[i];
        if (p == "~" || p == "." || p == "..") continue;
        if (!last && p.front() == '.') continue;   // .config, .local, .claude on the way
        const std::string lp = lower_ascii(p);
        if (is_generic_path_word(lp) || (!user_name().empty() && lp == user_name())) continue;
        float w = last ? 1.0f : 1.3f;   // a project folder names the topic more than a file does
        if (last) {
            if (const size_t dot = p.rfind('.'); dot != std::string::npos && dot > 0 && p.size() - dot <= 6) {
                for (const auto& t : terms_of(p.substr(dot + 1))) out.push_back({t, 0.8f});
                p.resize(dot);
            }
        }
        for (const auto& t : terms_of(p)) out.push_back({t, w});
    }
}

}  // namespace

std::string Distiller::prose(const std::string& raw, size_t max_chars) {
    std::string s = drop_between(raw, "<pasted_content", "</pasted_content>");
    s = drop_between(s, "```", "```");
    std::string keep;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t e = s.find('\n', pos);
        if (e == std::string::npos) e = s.size();
        const std::string line = s.substr(pos, e - pos);
        if (!terminal_line(line)) keep += line + "\n";
        pos = e + 1;
    }
    s = squash(without_urls(keep));
    if (s.size() > max_chars) s = utf8_head(s, max_chars * 2 / 3) + " " + utf8_tail(s, max_chars / 3);   // the ask is often at the end
    return s;
}

Query Distiller::pick(std::vector<Term> all, std::string text, const IdfFn& idf) const {
    std::map<std::string, float> merged;
    for (const auto& t : all) {
        auto it = merged.find(t.text);
        if (it == merged.end()) merged[t.text] = t.weight;
        else it->second = std::min(2.0f, it->second + 0.1f * t.weight);   // said twice: a little more
    }
    std::vector<std::pair<std::string, float>> ranked(merged.begin(), merged.end());
    auto value = [&](const std::pair<std::string, float>& p) {
        if (idf) return p.second * idf(p.first);   // an unknown term (0) cannot match anything
        return p.second * static_cast<float>(std::min<size_t>(p.first.size(), 10)) / 5.0f;
    };
    std::stable_sort(ranked.begin(), ranked.end(), [&](const auto& a, const auto& b) { return value(a) > value(b); });
    Query q;
    q.text = std::move(text);
    for (const auto& r : ranked) {
        if (q.terms.size() >= opt_.max_terms) break;
        if (idf && idf(r.first) <= 0.0f) continue;
        q.terms.push_back({r.first, r.second});
    }
    return q;
}

Query Distiller::text(const std::string& s, const IdfFn& idf) const {
    const std::string p = prose(s, opt_.max_chars);
    std::vector<Term> all;
    size_t i = 0;
    while (i < p.size()) {
        while (i < p.size() && p[i] == ' ') ++i;
        size_t j = i;
        while (j < p.size() && p[j] != ' ') ++j;
        if (j > i) {
            std::string w = p.substr(i, j - i);
            float boost = 1.0f;
            auto wrapped = [&](char c) { return w.size() > 2 && w.front() == c && w.back() == c; };
            if (wrapped('`') || wrapped('"') || wrapped('\'')) {   // quoted: said on purpose
                w = w.substr(1, w.size() - 2);
                boost = 1.5f;
            }
            if (w.find('/') != std::string::npos) {
                path_terms(w, all);
            } else {
                const bool ident = w.find('-') != std::string::npos || w.find('_') != std::string::npos ||
                                   std::any_of(w.begin(), w.end(), is_digit) ||
                                   (std::any_of(w.begin() + 1, w.end(), is_upper) && std::any_of(w.begin(), w.end(), is_lower));
                for (const auto& t : terms_of(w)) all.push_back({t, boost * (ident ? 1.2f : 1.0f)});
            }
        }
        i = j;
    }
    return pick(std::move(all), p, idf);
}

Query Distiller::terms(const std::vector<std::pair<std::string, float>>& pieces, const IdfFn& idf) const {
    std::vector<Term> all;
    std::string text;
    for (const auto& [piece, weight] : pieces) {
        text += (text.empty() ? "" : " ") + piece;
        for (const auto& t : terms_of(piece)) all.push_back({t, weight});
    }
    return pick(std::move(all), std::move(text), idf);
}

}  // namespace know
}  // namespace dd
