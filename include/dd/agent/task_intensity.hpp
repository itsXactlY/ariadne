#pragma once
// dd/agent/task_intensity.hpp — port of agent/task_intensity.py PURE functions.
// Ported: GAUGE_LEVELS, DEFAULT_FLOOR, _LEVEL_INDEX, _SCORE_LEVELS, all *_RE/*_TERMS
//   tables, _build_hit, _continuation_hit, _text_of, _last_user_text,
//   _tool_stats, score_task, level_for_score, clamp_level, _task_hits, estimate_level.
// NOT ported: adjust_agent_reasoning (mutates an agent object + calls _vprint — impure).
//
// UNICODE CAVEAT (std::regex is byte-based, ECMA, ASCII \w/\S):
//   * re.IGNORECASE does NOT fold non-ASCII: umlaut terms (gründlich, lück, überlege,
//     schwachstell, denk gründlich, ...) match byte-exact but NOT case-insensitively.
//   * Multibyte markers •(U+2022) in the bullet class and ❌(U+274C)/✗(U+2717) in
//     _ERROR_RE are kept verbatim but will not match their codepoint.
//   Ground-truth tests use ASCII-dominant inputs; the gap is documented, NOT papered.
#include "dd/core/json.hpp"
#include <string>
#include <vector>
#include <regex>
#include <utility>
#include <algorithm>
#include <cctype>

namespace dd {
namespace agent {
using namespace core;

static constexpr const char* DEFAULT_FLOOR = "low";
static constexpr const char* GAUGE_LEVELS[] = {"low", "medium", "high", "xhigh", "max", "ultra"};
constexpr const int N_GAUGE = static_cast<int>(sizeof(GAUGE_LEVELS)/sizeof(GAUGE_LEVELS[0]));

static int level_index(const std::string& name) {
    for (int i = 0; i < N_GAUGE; ++i)
        if (std::string(GAUGE_LEVELS[i]) == name) return i;
    return -1;
}

struct ScoreLevel { int threshold; const char* level; };
static const ScoreLevel SCORE_LEVELS[] = {
    {42, "ultra"}, {14, "max"}, {10, "xhigh"}, {7, "high"}, {4, "medium"}, {0, "low"}
};

static std::string re_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '\\': case '^': case '$': case '.': case '[': case ']':
            case '(': case ')': case '|': case '?': case '*': case '+':
            case '{': case '}': out.push_back('\\');
            default: break;
        }
        out.push_back(c);
    }
    return out;
}

// Build `\b(...)\b` alternation from a term list.
static std::string alt_word(const std::vector<std::string>& terms, const std::string& rfx = "\\b") {
    std::string p = "\\b(";
    for (size_t i = 0; i < terms.size(); ++i) { if (i) p += "|"; p += re_escape(terms[i]); }
    return p + ")" + rfx;
}
// Build `\b(?:...)` (stem prefix: word-boundary left, no right \b).
static std::string alt_prefix(const std::vector<std::string>& terms) {
    std::string p = "\\b(?:";
    for (size_t i = 0; i < terms.size(); ++i) { if (i) p += "|"; p += re_escape(terms[i]); }
    return p + ")";
}
// Build `^\s*(?:...)` (chat-ack / error anchors).
static std::string alt_anchor(const std::vector<std::string>& terms) {
    std::string p = "^\\s*(?:";
    for (size_t i = 0; i < terms.size(); ++i) { if (i) p += "|"; p += re_escape(terms[i]); }
    return p + ")";
}

// ---- term tables (verbatim from Python) ----
static const std::vector<std::string> TASK_TERMS = {
    "audit","refactor","redesign","architect","architecture",
    "debug","root cause","root-cause","troubleshoot","investigate",
    "implement","deploy","migrate","migration","restructure",
    "optimize","benchmark","security","vulnerability","exploit",
    "deep dive","deep-dive","comprehensive","thorough","exhaustive",
    "gr\u00fcndlich","vollst\u00e4ndlich","komplett","architektur","sicherheit",
    "autonomous","autonomously","from scratch","end to end",
    "end-to-end","production-ready","production ready"};
static const std::vector<std::string> TASK_STEMS = {
    "auditier","analysier","untersuch","debug","refaktorier",
    "implementier","entwickl","migrier","optimier","sicher",
    "bau","komplett","schwachstell","l\u00fcck"};
static const std::vector<std::string> BUILD_VERBS = {
    "write","build","create","generate","produce","ship",
    "rewrite","redo","put together","spin up","stand up"};
static const std::vector<std::string> DELIVERABLE_NOUNS = {
    "website","site","app","application","page","system",
    "service","pipeline","dashboard","api","tool","script",
    "plugin","extension","bot","agent","server","database"};
static const std::vector<std::string> CONTINUATION_TERMS = {
    "status","continue","keep going","proceed","go on",
    "any update","progress","still there","you there",
    "weiter","geht's voran","wie ist der stand","stand der dinge"};
static const std::vector<std::string> DEPTH_TERMS = {
    "think hard","thinking hard","reason deeply","deep reasoning",
    "reason about","denk gr\u00fcndlich","denke gr\u00fcndlich","\u00fcberlege gr\u00fcndlich",
    "denk tief","denke tief","tief nachdenken","deep think",
    "deep audit","full audit","deep dive","deep-dive"};
static const std::vector<std::string> MAX_DEPTH_TERMS = {
    "use maximum reasoning","max reasoning","ultra reasoning",
    "maximales reasoning","maximum reasoning","ultra"};
static const std::vector<std::string> CHAT_ACKS = {
    "danke","ok","okay","gut","weiter","genau","aha","cool",
    "perfekt","super","nice","thanks","thx","ja","nein","yep",
    "sure","fine","great","klingt gut","alles klar","verstanden"};

// ---- compiled regexes (function-local statics; icase mirrors re.IGNORECASE) ----
static const std::regex& RE_TASK() {
    static const std::regex r(alt_word(TASK_TERMS), std::regex_constants::icase); return r;
}
static const std::regex& RE_TASK_STEM() {
    static const std::regex r(alt_prefix(TASK_STEMS), std::regex_constants::icase); return r;
}
static const std::regex& RE_BUILD_VERB() {
    static const std::regex r(alt_word(BUILD_VERBS), std::regex_constants::icase); return r;
}
// `\b(... )s?\b` — plural noun tail
static const std::regex& RE_DELIVERABLE() {
    static const std::regex r(alt_word(DELIVERABLE_NOUNS, "s?\\b"), std::regex_constants::icase); return r;
}
static const std::regex& RE_CONTINUATION() {
    static const std::regex r(alt_word(CONTINUATION_TERMS), std::regex_constants::icase); return r;
}
static const std::regex& RE_DEPTH() {
    static const std::regex r(alt_word(DEPTH_TERMS), std::regex_constants::icase); return r;
}
static const std::regex& RE_MAX_DEPTH() {
    static const std::regex r(alt_word(MAX_DEPTH_TERMS), std::regex_constants::icase); return r;
}
static const std::regex& RE_CHAT_ACK() {
    static const std::regex r(alt_anchor(CHAT_ACKS), std::regex_constants::icase); return r;
}
static const std::regex& RE_CODE() {
    static const std::regex r("```|/\\w[\\w./-]*\\.(?:py|sh|js|ts|yaml|yml|json|md|cpp|h)\\b|\\.git\\b|branch|repo",
        std::regex_constants::icase);
    return r;
}
static const std::regex& RE_PATH() {
    static const std::regex r("(?:/home/|/etc/|/var/|/usr/|~/)", std::regex_constants::icase);
    return r;
}
static const std::regex& RE_ERROR() {
    // ❌(U+274C) ✗(U+2717) kept as raw UTF-8 bytes; byte regex won't fold those codepoints.
    static const std::regex r("^(?:Error|Traceback|Exception|FAILED|Failed|\xe2\x9d\x8c|\xe2\x9c\x97|timed out|timeout)",
        std::regex_constants::icase);
    return r;
}

static bool rx_search(const std::regex& r, const std::string& s) {
    return std::regex_search(s, r);
}
static int rx_count(const std::regex& r, const std::string& s) {
    int n = 0;
    for (auto it = std::sregex_iterator(s.begin(), s.end(), r); it != std::sregex_iterator(); ++it) ++n;
    return n;
}

// clamp_level(level, floor)
static std::string clamp_level(std::string level, std::string floor) {
    if (level_index(level) < 0) level = DEFAULT_FLOOR;
    if (level_index(floor) < 0) floor = DEFAULT_FLOOR;
    if (level_index(level) < level_index(floor)) return floor;
    return level;
}

// level_for_score(score, floor)
static std::string level_for_score(int score, const std::string& floor = DEFAULT_FLOOR) {
    std::string lvl = "low";
    for (const auto& sl : SCORE_LEVELS) {
        if (score >= sl.threshold) { lvl = sl.level; break; }
    }
    return clamp_level(lvl, floor);
}

// _text_of(message dict)
static std::string _text_of(const Json& message) {
    const Json* content = json_get(message, "content");
    if (!content) return "";
    if (content->type == Json::Str) return content->str;
    if (content->type == Json::Arr) {
        std::vector<std::string> parts;
        for (const auto& part : content->arr) {
            if (part->type == Json::Obj) {
                const Json* t = json_get(*part, "text");
                if (t && t->type == Json::Str) { parts.push_back(t->str); }
                else {
                    const Json* c = json_get(*part, "content");
                    if (c && c->type == Json::Str) parts.push_back(c->str);
                }
            } else if (part->type == Json::Str) {
                parts.push_back(part->str);
            }
        }
        std::string joined;
        for (size_t i = 0; i < parts.size(); ++i) { if (i) joined += "\n"; joined += parts[i]; }
        return joined;
    }
    return "";
}

// _last_user_text(messages)
static std::string _last_user_text(const std::vector<Json>& messages) {
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->type == Json::Obj) {
            const Json* role = json_get(*it, "role");
            if (role && role->type == Json::Str && role->str == "user") return _text_of(*it);
        }
    }
    return "";
}

// _tool_stats(messages) -> (tools, errors)
static std::pair<int,int> _tool_stats(const std::vector<Json>& messages) {
    int tools = 0, errors = 0;
    for (const auto& msg : messages) {
        if (msg.type != Json::Obj) continue;
        const Json* role = json_get(msg, "role");
        if (!(role && role->type == Json::Str && role->str == "tool")) continue;
        tools += 1;
        const Json* content = json_get(msg, "content");
        if (content && content->type == Json::Str) {
            std::string s = content->str;
            size_t a = 0, b = s.size();
            while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
            while (b > a && std::isspace(static_cast<unsigned char>(s[b-1]))) --b;
            std::string stripped = s.substr(a, b - a);
            if (rx_search(RE_ERROR(), stripped)) errors += 1;
        }
    }
    return {tools, errors};
}

// _continuation_hit(text, messages)
static bool _continuation_hit(const std::string& text, const std::vector<Json>* messages) {
    if (!rx_search(RE_CONTINUATION(), text)) return false;
    int tools = messages ? _tool_stats(*messages).first : 0;
    return tools > 0;
}

// _build_hit(text)
static bool _build_hit(const std::string& text) {
    if (!rx_search(RE_BUILD_VERB(), text)) return false;
    return rx_search(RE_PATH(), text) || rx_search(RE_DELIVERABLE(), text);
}

// _task_hits(text, messages)
static int _task_hits(const std::string& text, const std::vector<Json>* messages) {
    int hits = rx_count(RE_TASK(), text) + rx_count(RE_TASK_STEM(), text);
    if (_build_hit(text)) hits += 2;
    if (messages && _continuation_hit(text, messages)) hits += 1;
    return hits;
}

// score_task(user_text, messages=nullptr)
static int score_task(const std::string& user_text, const std::vector<Json>* messages = nullptr) {
    const std::string& text = user_text;
    int score = 0;
    // words = len(re.findall(r"\S+", text))
    int words = 0; bool inrun = false;
    for (unsigned char c : text) {
        bool ws = std::isspace(c);
        if (!ws) { if (!inrun) { ++words; inrun = true; } }
        else inrun = false;
    }
    if (words > 300) score += 3;
    else if (words > 120) score += 2;
    else if (words > 40) score += 1;

    // bullets: r"(?:^|\n)\s*(?:[-*•]|\d+[.)])\s+" — •(U+2022) gap documented
    static const std::regex BUL("(?:^|\n)\\s*(?:[-*]|\\xe2\\x80\\xa2|\\d+[.)])\\s+", std::regex::icase);
    int bullets = rx_count(BUL, text);
    score += std::min(bullets / 3, 3);

    int task_hits = _task_hits(text, messages);
    score += std::min(task_hits, 8) * 2;
    score += std::min(rx_count(RE_DEPTH(), text), 8) * 8;
    if (rx_search(RE_CODE(), text)) score += 1;
    if (rx_search(RE_PATH(), text)) score += 1;

    auto tp = _tool_stats(messages ? *messages : std::vector<Json>{});
    int tools = tp.first, errors = tp.second;
    if (tools > 8) score += 3;
    else if (tools > 3) score += 2;
    else if (tools > 0) score += 1;
    score += std::min(errors, 3);

    if (words <= 25 && rx_search(RE_CHAT_ACK(), text)) score -= 1;
    if (words <= 5 && rx_search(RE_CHAT_ACK(), text)) score -= 1;

    return std::max(score, 0);
}

// estimate_level(user_text, messages=nullptr, floor=DEFAULT_FLOOR)
static std::string estimate_level(const std::string& user_text,
                                  const std::vector<Json>* messages = nullptr,
                                  const std::string& floor = DEFAULT_FLOOR) {
    int score = score_task(user_text, messages);
    std::string level = level_for_score(score, floor);
    const std::string& text = user_text;
    int hits = _task_hits(text, messages);
    if (rx_search(RE_MAX_DEPTH(), text)) level = clamp_level(level, "max");
    else if (rx_search(RE_DEPTH(), text)) level = clamp_level(level, "high");
    else if (hits >= 6) level = clamp_level(level, "xhigh");
    else if (hits >= 3) level = clamp_level(level, "high");
    else if (hits >= 1) level = clamp_level(level, "medium");
    return level;
}

} // agent
} // dd
