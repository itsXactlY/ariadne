// dd::know::terms_of — the tokenizer (SIMD classification, a fast path for clean ASCII words, views instead of copies)
// gives exactly what the plain per-character one gave. The old one is kept here verbatim as the reference; the two are
// compared on structured cases, on every position across the 32- and 64-byte windows, and on random text with
// UTF-8, broken bytes, camelCase, versions and error codes, at every kernel level the CPU has.
#include <cstdint>
#include <cstdio>
#include <cctype>
#include <random>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
#include <algorithm>

#include "dd/know/scan.hpp"
#include "dd/know/text.hpp"

namespace ref {

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


bool is_stopword_ref(std::string_view w) { return stopwords().count(w) > 0; }

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


std::vector<std::string> terms_of_ref(std::string_view text, bool keep_stop) {
    std::vector<std::string> out;
    std::string cur, run;   // cur: the part (camelCase split); run: the whole alphanumeric run it sits in
    bool split_in_run = false;
    auto emit = [&](const std::string& tok) {
        const bool digits = std::all_of(tok.begin(), tok.end(), is_digit);
        const bool keep = digits ? tok.size() >= 3 : tok.size() >= 2;
        if (keep && (keep_stop || !is_stopword_ref(tok))) out.push_back(stem(tok));
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


}  // namespace ref

using namespace dd::know;

static int g_fail = 0;
static void chk(bool ok, const std::string& label) {
    if (!ok) {
        ++g_fail;
        if (g_fail <= 12) std::printf("FAIL %s\n", label.c_str());
    }
}

static std::string show(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& t : v) s += (s.empty() ? "" : "|") + t;
    return s;
}

static long g_cases = 0;
static void same(const std::string& text, const std::string& tag) {
    for (bool keep_stop : {false, true}) {
        ++g_cases;
        const auto want = ref::terms_of_ref(text, keep_stop);
        const auto got = terms_of(text, keep_stop);
        if (want != got) {
            std::string esc;
            for (unsigned char c : text.substr(0, 120)) esc += (c >= 0x20 && c < 0x7F) ? std::string(1, static_cast<char>(c)) : "\\x" + std::to_string(c);
            chk(false, tag + " keep_stop=" + (keep_stop ? "1" : "0") + " text [" + esc + "]\n   want " + show(want) + "\n   got  " + show(got));
        }
    }
}

static const char* kWords[] = {"camelCase", "HTTPServer", "BTQuant", "readFile", "v1.2.3", "E0433", "ERR_CONNECTION", "0.1.45", "3.14.7-rc1", "x86_64", "UTF8",
                               "iPhone", "a1B2c3", "podman", "Podman", "PODMAN", "the", "and", "Und", "fuer", "deployed", "containers", "caches", "running",
                               "ies", "ss", "123", "12", "9", "v2", "V2Ray", "ENOENT", "SIGSEGV", "naïve", "Müller", "über", "straße", "café", "日本語", "😀",
                               "—", "→", "·", "ÄÖÜ", "ÉCOLE", "Ångström", "x", "ab", "a-b", "a_b", "a.b", "a/b/c", "(quoted)", "\"x\"", "[v1.0]", "end.", "wow!", "why?"};

static std::string random_text(std::mt19937& rng, size_t max_len) {
    std::string s;
    const std::string punct = " \t\n\r.,;:!?()[]{}<>\"'`-_+/\\@#%&*=~^|$";
    const std::string letters = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    const size_t target = rng() % (max_len + 1);
    while (s.size() < target) {
        switch (rng() % 10) {
            case 0: case 1: case 2: s += kWords[rng() % (sizeof kWords / sizeof *kWords)]; break;
            case 3: case 4: case 5: for (int k = rng() % 12; k >= 0; --k) s.push_back(letters[rng() % letters.size()]); break;
            case 6: case 7: s.push_back(punct[rng() % punct.size()]); break;
            case 8: s.push_back(' '); break;
            default: {   // broken or stray bytes
                static const unsigned char junk[] = {0x80, 0xBF, 0xC3, 0xE2, 0xFF, 0xF0, 0xC0, 0xA4};
                s.push_back(static_cast<char>(junk[rng() % sizeof junk]));
            }
        }
    }
    return s;
}

static void run_level(const char* level) {
    if (!simd::force(level)) {
        std::printf("skip %s (cpu lacks it)\n", level);
        return;
    }
    std::printf("tokenizer on %s kernels\n", simd::level());
    const std::string tag = level;

    // structured: each word alone, after a prefix of every length across the windows, and before a suffix
    for (const char* w : kWords) {
        same(w, tag + " alone " + w);
        for (size_t pre = 0; pre <= 130; ++pre) {
            same(std::string(pre, ' ') + w, tag + " space-prefix " + std::to_string(pre) + " " + w);
            same(std::string(pre, 'z') + " " + w + " tail", tag + " word-prefix " + std::to_string(pre) + " " + w);
        }
        for (size_t suf = 0; suf <= 70; suf += 1) same(std::string(w) + std::string(suf, '.') + " x", tag + " dots " + std::to_string(suf) + " " + w);
    }
    // long alphanumeric runs around the 32 and 64 limits, with and without a seam
    for (size_t len = 20; len <= 140; ++len) {
        same(std::string(len, 'a'), tag + " run " + std::to_string(len));
        same(std::string(len, 'A'), tag + " caps " + std::to_string(len));
        same(std::string(len, 'a') + "B" + std::string(3, 'c'), tag + " seam " + std::to_string(len));
        same(std::string(len, 'a') + "\xc3\xa4", tag + " umlaut after run " + std::to_string(len));
        same("\xc3\xa4" + std::string(len, 'a'), tag + " umlaut before run " + std::to_string(len));
        same(std::string(len, '7') + " v1.2.3 " + std::string(len, 'x'), tag + " digits " + std::to_string(len));
    }
    // random text
    std::mt19937 rng(20261022);
    for (int it = 0; it < 120000; ++it) same(random_text(rng, it % 10 == 0 ? 600 : 160), tag + " random " + std::to_string(it));
}

int main() {
    const std::string best = simd::level();
    run_level("scalar");
    run_level("avx2");
    run_level("avx512");
    simd::force(best.c_str());
    // the stopword table answers like the set for words the list has and ones near them
    for (const char* w : {"the", "and", "um", "fuer", "ueber", "irgendwie", "everything", "yourself", "theirs", "wanna", "dont", "a", "ok", "th", "thee", "andd", "zzz", ""})
        chk(is_stopword(w) == ref::is_stopword_ref(w), std::string("stopword ") + w);
    for (size_t len = 1; len <= 40; ++len) chk(is_stopword(std::string(len, 'a')) == ref::is_stopword_ref(std::string(len, 'a')), "stopword run " + std::to_string(len));
    std::printf("%ld comparisons\n", g_cases);
    if (g_fail) std::printf("%d FAILED\n", g_fail);
    else std::printf("all know tokenizer tests passed\n");
    return g_fail ? 1 : 0;
}
