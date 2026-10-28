// src/ui_grammar.cpp — the language tables the transcript's code is coloured from.
//
// Which word is a keyword is a fact about a language, not a fact the harness may assert. Every
// table here is read off disk: a highlight.yaml under a skill root, written by whoever owns that
// skill. Nothing in this file names a language. A fence whose tag no file defines colours the
// facts every language shares — strings, numbers, comments — and claims nothing else, which is
// the same refusal that makes a prose '-' a bullet rather than a deletion.
#include "dd/ui.hpp"
#include "dd/core/config.hpp"
#include "dd/core/json.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace dd {
namespace ui {
namespace {

// A word as the file wrote it and as a fence names it: quotes, case and padding are noise.
std::string word_of(std::string s) {
    const size_t a = s.find_first_not_of(" \t\r\n\"'");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n\"'");
    s = s.substr(a, b - a + 1);
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// A list, or one string that lists: both are how a skill file spells a set of words.
std::vector<std::string> words_of(const core::Json* j) {
    std::vector<std::string> out;
    if (!j) return out;
    if (j->type == core::Json::Arr) {
        for (const auto& p : j->arr)
            if (p && p->type == core::Json::Str) {
                const std::string w = word_of(p->str);
                if (!w.empty()) out.push_back(w);
            }
    } else if (j->type == core::Json::Str) {
        std::string cur;
        for (const char ch : j->str) {
            if (ch == ',' || ch == ' ' || ch == '\n' || ch == '\t') {
                const std::string w = word_of(cur);
                if (!w.empty()) out.push_back(w);
                cur.clear();
            } else {
                cur.push_back(ch);
            }
        }
        const std::string w = word_of(cur);
        if (!w.empty()) out.push_back(w);
    }
    // Sorted once, so a tokenizer's lookup is a binary search and two files that name the same
    // word are one word.
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// Quote characters, kept exactly as written. word_of strips quotes as noise, and here the quote IS
// the datum: a language whose strings use one delimiter (json) must not inherit another's, and a
// file that declares none at all still gets the two every language shares. A multi-character
// delimiter keeps its whole spelling; the row scan reads its first character.
std::vector<std::string> chars_of(const core::Json* j) {
    std::vector<std::string> out;
    if (!j) return out;
    auto add = [&](const std::string& raw) {
        const size_t a = raw.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return;
        const size_t b = raw.find_last_not_of(" \t\r\n");
        out.push_back(raw.substr(a, b - a + 1));
    };
    if (j->type == core::Json::Arr) {
        for (const auto& p : j->arr)
            if (p && p->type == core::Json::Str) add(p->str);
    } else if (j->type == core::Json::Str) {
        std::string cur;
        for (const char ch : j->str) {
            if (ch == ',' || ch == ' ' || ch == '\n' || ch == '\t') { add(cur); cur.clear(); continue; }
            cur.push_back(ch);
        }
        add(cur);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

uint32_t hue_of(const std::string& v) {
    if (v.size() != 7 || v[0] != '#') return kNone;
    char* end = nullptr;
    const unsigned long hex = std::strtoul(v.c_str() + 1, &end, 16);
    if (!end || *end != '\0') return kNone;
    return rgb(static_cast<uint32_t>(hex));
}

// A grammar file is a few hundred lines. Reading it whole is the honest cost: a truncated parse
// would colour half a language, which is worse than not colouring it at all.
std::string slurp(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::string text;
    char buf[8192];
    for (;;) {
        f.read(buf, sizeof buf);
        text.append(buf, static_cast<size_t>(f.gcount()));
        if (!f) break;
        if (text.size() > 128u * 1024u) return {};   // absurd file: refuse to parse it
    }
    return text;
}

CodeTok tok_of(const std::string& name, bool& ok) {
    ok = true;
    if (name == "keyword") return CodeTok::Keyword;
    if (name == "type") return CodeTok::Type;
    if (name == "builtin") return CodeTok::Builtin;
    if (name == "function" || name == "func") return CodeTok::Func;
    if (name == "string") return CodeTok::String;
    if (name == "number") return CodeTok::Number;
    if (name == "comment") return CodeTok::Comment;
    ok = false;
    return CodeTok::Comment;
}

Grammar grammar_of(const core::Config& c, const std::string& origin) {
    Grammar g;
    g.name = word_of(c.get_string("language"));
    if (g.name.empty()) g.name = word_of(c.get_string("name"));
    g.aliases = words_of(c.get("aliases"));
    g.keywords = words_of(c.get("keywords"));
    g.types = words_of(c.get("types"));
    g.builtins = words_of(c.get("builtins"));
    // An undeclared key is not a declared-none: html has no line comment and writes `line_comment:
    // []`, which must colour nothing rather than colour the whole row (an empty prefix matches
    // everywhere). A file that says nothing gets what every language shares.
    if (c.has("line_comment") || c.has("line_comments")) {
        g.line_comment = words_of(c.get("line_comment"));
        if (g.line_comment.empty()) g.line_comment = words_of(c.get("line_comments"));
    } else {
        g.line_comment = generic_grammar().line_comment;
    }
    g.preproc = words_of(c.get("preproc"));
    if (c.has("quotes") || c.has("strings")) {
        g.quotes = chars_of(c.get("quotes"));
        if (g.quotes.empty()) g.quotes = chars_of(c.get("strings"));
    }
    else
        g.quotes = {std::string("\""), std::string("'")};
    if (c.has("numbers")) g.numbers = c.get_bool("numbers");
    if (c.has("keys_are_types")) g.keys_are_types = c.get_bool("keys_are_types");
    if (c.has("capital_is_type")) g.capital_is_type = c.get_bool("capital_is_type");
    // Whether this language's rows are patch rows. The file says it; the C++ carries no list of
    // patch-format names, so a second unified-diff dialect is a line of data, not a rebuild.
    if (c.has("diff")) g.diff_rules = c.get_bool("diff");
    if (const core::Json* pair = c.get_map("block_comment"); pair && pair->type == core::Json::Obj) {
        const std::string open = c.get_string("block_comment.open");
        const std::string close = c.get_string("block_comment.close");
        if (!open.empty() && !close.empty()) g.block_comment.emplace_back(open, close);
    }
    if (const core::Json* pairs = c.get_list("block_comments"); pairs && pairs->type == core::Json::Arr) {
        for (const auto& p : pairs->arr) {
            if (!p || p->type != core::Json::Obj) continue;
            const core::Config sub{*p};
            const std::string open = sub.get_string("open");
            const std::string close = sub.get_string("close");
            if (!open.empty() && !close.empty()) g.block_comment.emplace_back(open, close);
        }
    }
    if (const core::Json* m = c.get_map("colors"); m && m->type == core::Json::Obj) {
        for (const auto& [key, val] : m->obj) {
            bool ok = false;
            const CodeTok t = tok_of(word_of(key), ok);
            if (!ok || !val || val->type != core::Json::Str) continue;
            const uint32_t hue = hue_of(val->str);
            if (hue != kNone) g.colors.emplace_back(t, hue);
        }
    }
    // A language no fence can name is dead data; saying so is the file's problem, not the
    // screen's. It is dropped rather than guessed at from the path's basename.
    if (g.name.empty()) return g;
    (void)origin;
    return g;
}

}  // namespace

uint32_t Grammar::color(CodeTok t, uint32_t fallback) const {
    for (const auto& [k, v] : colors)
        if (k == t && v != kNone) return v;
    return fallback;
}

const Grammar& generic_grammar() {
    // Strings, numbers and comments are facts in every language. No keyword and no type: a word
    // no file declared a keyword for is not a keyword because this screen happens to be running.
    static const Grammar g = [] {
        Grammar t;
        t.name = "text";
        t.line_comment = {"#", "//", "--", ";"};
        t.quotes = {"\"", "'"};
        t.numbers = true;
        t.capital_is_type = false;
        t.keys_are_types = false;
        return t;
    }();
    return g;
}

void GrammarSet::add(Grammar g) {
    // The last file to name a language wins, and a name is claimed exactly once: find() must
    // never have to choose between two files, and the operator's own tree may correct a default.
    for (auto& e : langs_)
        if (e.name == g.name) { e = std::move(g); return; }
    langs_.push_back(std::move(g));
}

const Grammar* GrammarSet::find(const std::string& lang) const {
    const std::string n = word_of(lang);
    if (n.empty()) return nullptr;
    for (const auto& g : langs_) {
        if (g.name == n) return &g;
        for (const auto& a : g.aliases)
            if (a == n) return &g;
    }
    return nullptr;
}

std::vector<std::string> GrammarSet::names() const {
    std::vector<std::string> out;
    out.reserve(langs_.size());
    for (const auto& g : langs_) out.push_back(g.name);
    std::sort(out.begin(), out.end());
    return out;
}

GrammarSet GrammarSet::from_yaml(const std::string& text, const std::string& origin) {
    GrammarSet set;
    set.sources_.push_back(origin);
    core::Json root = core::parse_yaml(text);
    if (root.type != core::Json::Obj) return set;
    const core::Config c{root};
    // `languages:` is a map keyed by language name, because the YAML subset this parser reads has
    // no block list of inline maps: `- language: x` comes out as the string "language: x". A file
    // written as a list elsewhere still loads; the key names the language when the body omits it.
    if (const core::Json* m = c.get_map("languages"); m && m->type == core::Json::Obj) {
        for (const auto& [key, val] : m->obj) {
            if (!val || val->type != core::Json::Obj) continue;
            Grammar g = grammar_of(core::Config{*val}, origin);
            if (g.name.empty()) g.name = word_of(key);
            if (!g.name.empty()) set.add(std::move(g));
        }
        return set;
    }
    if (const core::Json* list = c.get_list("languages");
        list && list->type == core::Json::Arr) {
        // One file may define several languages; a skill that owns three of them is one file.
        for (const auto& item : list->arr) {
            if (!item || item->type != core::Json::Obj) continue;
            Grammar g = grammar_of(core::Config{*item}, origin);
            if (!g.name.empty()) set.add(std::move(g));
        }
        return set;
    }
    Grammar g = grammar_of(c, origin);
    if (!g.name.empty()) set.add(std::move(g));
    return set;
}

GrammarSet GrammarSet::load(const std::vector<std::filesystem::path>& roots) {
    GrammarSet set;
    size_t files = 0;
    std::error_code ec;
    for (const auto& root : roots) {
        if (!std::filesystem::is_directory(root, ec)) continue;
        for (std::filesystem::recursive_directory_iterator it(root,
                                                             std::filesystem::directory_options::skip_permission_denied, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (it->is_directory(ec)) continue;
            if (it->path().filename() != "highlight.yaml") continue;
            const std::string text = slurp(it->path());
            if (text.empty()) continue;
            ++files;
            GrammarSet one = GrammarSet::from_yaml(text, it->path().string());
            for (auto& g : one.langs_) set.add(std::move(g));
            set.sources_.push_back(it->path().string());
        }
    }
    if (files == 0) set.sources_.push_back("no highlight.yaml under the skill roots");
    return set;
}

void GrammarSet::reconcile(const Theme& th) {
    // A grammar's hues are measured against the same ground the theme's palette is measured
    // against: rules are data too, and a hue this terminal cannot show colours nothing. The count
    // is kept because a palette that had to move is a fact the operator is owed, not a bug to
    // hide — a skin that asks for white on white is a skin, and it did not ask to be unreadable.
    for (auto& g : langs_)
        for (auto& [tok, hue] : g.colors) {
            const uint32_t got = th.readable(hue);
            if (got == hue) continue;
            // A hue with no chroma has nothing left to preserve: nudged toward the ground's pole it
            // lands on the same grey every other unreadable kind lands on, and the block colours as
            // one run -- the thing a grammar file exists to prevent. Such a claim falls back to the
            // theme's reconciled hue for that token kind; a chromatic hue keeps its hue and only
            // loses luminance.
            const bool achromatic = ((hue >> 16) & 0xFF) == ((hue >> 8) & 0xFF) &&
                                    ((hue >> 8) & 0xFF) == (hue & 0xFF);
            hue = achromatic ? th.code_hue(tok) : got;
            ++fixed_;
        }
}

uint32_t code_color(const Theme& th, const Grammar& g, CodeTok t, uint32_t def) {
    // Order of authority: what the skin explicitly set (colors.code_*), then what the language
    // file declares, then the palette default. `def` is the skin's own value, so a skin keeps its
    // colours — a language file decides structure, never who owns the screen.
    if (th.skin_set(t)) return def;
    return g.color(t, def);
}

}  // namespace ui
}  // namespace dd
