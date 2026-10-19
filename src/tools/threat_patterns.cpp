// dd::tools::threat_patterns — port of tools/threat_patterns.py.
// NFKC normalization via ICU (unicode/unorm.h, UNORM_NFKC).
#include <cstdint>
#include <cstring>
#include <regex>
#include <string>
#include <vector>
#include <optional>
#include <unordered_set>
#include <unordered_map>
#include <unicode/uversion.h>
#include <unicode/unorm2.h>

#include "dd/tools/threat_patterns.hpp"

namespace dd {
namespace tools {

namespace {

constexpr int MAX_SCAN_CHARS = 65536;

// ---- UTF codecs -----------------------------------------------------------
// UTF-8 (std::string) <-> UTF-16 (UChar* / std::u16string).
std::u16string utf8_to_utf16(const std::string& s) {
    std::u16string out;
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c0 = (unsigned char)s[i];
        uint32_t cp = 0;
        int extra = 0;
        if (c0 < 0x80) { cp = c0; extra = 0; }
        else if ((c0 & 0xE0) == 0xC0) { cp = c0 & 0x1F; extra = 1; }
        else if ((c0 & 0xC0) == 0x80) { cp = 0xFFFD; extra = 0; }  // stray cont
        else if ((c0 & 0xF0) == 0xE0) { cp = c0 & 0x0F; extra = 2; }
        else if ((c0 & 0xF8) == 0xF0) { cp = c0 & 0x07; extra = 3; }
        else { cp = 0xFFFD; extra = 0; }
        ++i;
        for (int k = 0; k < extra && i < n; ++k, ++i) {
            unsigned char cc = (unsigned char)s[i];
            if ((cc & 0xC0) != 0x80) { cp = 0xFFFD; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (cp <= 0xFFFF) {
            out.push_back((char16_t)cp);
        } else if (cp <= 0x10FFFF) {
            cp -= 0x10000;
            out.push_back((char16_t)(0xD800 + (cp >> 10)));
            out.push_back((char16_t)(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(u'\uFFFD');
        }
    }
    return out;
}

std::string utf16_to_utf8(const UChar* u, int32_t len) {
    std::string out;
    for (int32_t i = 0; i < len; ++i) {
        uint32_t cp = (unsigned short)u[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len) {
            uint32_t lo = (unsigned short)u[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
        if (cp < 0x80) {
            out.push_back((char)cp);
        } else if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            out.push_back((char)(0xF0 | (cp >> 18)));
            out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

// NFKC normalize a UTF-8 string (mirrors unicodedata.normalize("NFKC", ...)).
std::string nfkc(const std::string& s) {
    std::u16string src = utf8_to_utf16(s);
    if (src.empty()) return std::string();
    UErrorCode ec = U_ZERO_ERROR;
    const UNormalizer2* norm = unorm2_getNFKCInstance(&ec);
    if (ec != U_ZERO_ERROR) return s;  // normalization unavailable: pass through
    int32_t needed = (int32_t)src.size() * 4 + 16;
    std::vector<UChar> buf((size_t)needed);
    ec = U_ZERO_ERROR;
    int32_t total = unorm2_normalize(norm, src.data(), (int32_t)src.size(),
                                     buf.data(), needed, &ec);
    if (ec == U_BUFFER_OVERFLOW_ERROR || total > needed) {
        needed = total + 1;
        buf.assign((size_t)needed, 0);
        ec = U_ZERO_ERROR;
        total = unorm2_normalize(norm, src.data(), (int32_t)src.size(),
                                 buf.data(), needed, &ec);
    }
    if (total < 0) total = 0;
    return utf16_to_utf8(buf.data(), total);
}

// Truncate a UTF-8 string to at most `max` codepoints (content[:MAX_SCAN_CHARS]).
std::string truncate_codepoints(const std::string& s, int max) {
    size_t i = 0, n = s.size();
    int count = 0;
    while (i < n && count < max) {
        unsigned char c0 = (unsigned char)s[i];
        size_t step = 1;
        if (c0 < 0x80) step = 1;
        else if ((c0 & 0xE0) == 0xC0) step = 2;
        else if ((c0 & 0xF0) == 0xE0) step = 3;
        else if ((c0 & 0xF8) == 0xF0) step = 4;
        i += step;
        ++count;
    }
    return s.substr(0, i);
}

// Invisible-unicode codepoints (INVISIBLE_CHARS frozenset).
const std::unordered_set<uint32_t>& invisible_chars() {
    static const std::unordered_set<uint32_t> s = {
        0x200b, 0x200c, 0x200d, 0x2060, 0x2062, 0x2063, 0x2064, 0xfeff,
        0x202a, 0x202b, 0x202c, 0x202d, 0x202e, 0x2066, 0x2067, 0x2068, 0x2069};
    return s;
}

// The (regex, pid, scope) table. `\'` in the Python source is written as a
// literal apostrophe here (ECMAScript rejects `\'` as an escape).
struct Pattern { const char* re; const char* pid; const char* scope; };
const std::vector<Pattern>& pattern_table() {
    static const std::vector<Pattern> t = {
        {R"(ignore\s+(?:\w+\s+){0,8}(previous|all|above|prior)\s+(?:\w+\s+){0,8}instructions)", "prompt_injection", "all"},
        {R"(system\s+prompt\s+override)", "sys_prompt_override", "all"},
        {R"(disregard\s+(?:\w+\s+){0,8}(your|all|any)\s+(?:\w+\s+){0,8}(instructions|rules|guidelines))", "disregard_rules", "all"},
        {R"(act\s+as\s+(if|though)\s+(?:\w+\s+){0,8}you\s+(?:\w+\s+){0,8}(have\s+no|don't\s+have)\s+(?:\w+\s+){0,8}(restrictions|limits|rules))", "bypass_restrictions", "all"},
        {R"(<!--[^>]{0,512}(?:ignore|override|system|secret|hidden)[^>]{0,512}-->)", "html_comment_injection", "all"},
        {R"(<\s*div\s+style\s*=\s*["'][^>]{0,2048}display\s*:\s*none)", "hidden_div", "all"},
        {R"(translate\s+[^\n]{0,512}\s+into\s+[^\n]{0,512}\s+and\s+(execute|run|eval))", "translate_execute", "all"},
        {R"(do\s+not\s+(?:\w+\s+){0,8}tell\s+(?:\w+\s+){0,8}the\s+user)", "deception_hide", "all"},

        {R"(you\s+are\s+(?:\w+\s+){0,8}now\s+(?:a|an|the)\s+)", "role_hijack", "context"},
        {R"(pretend\s+(?:\w+\s+){0,8}(you\s+are|to\s+be)\s+)", "role_pretend", "context"},
        {R"(output\s+(?:\w+\s+){0,8}(system|initial)\s+prompt)", "leak_system_prompt", "context"},
        {R"((respond|answer|reply)\s+without\s+(?:\w+\s+){0,8}(restrictions|limitations|filters|safety))", "remove_filters", "context"},
        {R"(you\s+have\s+been\s+(?:\w+\s+){0,8}(updated|upgraded|patched)\s+to)", "fake_update", "context"},
        {R"(\bname\s+yourself\s+\w+)", "identity_override", "context"},

        {R"(register\s+(as\s+)?a?\s*node)", "c2_node_registration", "context"},
        {R"((heartbeat|beacon|check[\s\-]?in)\s+(to|with)\s+)", "c2_heartbeat", "context"},
        {R"(pull\s+(down\s+)?(?:new\s+)?task(?:ing|s)?\b)", "c2_task_pull", "context"},
        {R"(connect\s+to\s+the\s+network\b)", "c2_network_connect", "context"},
        {R"(you\s+must\s+(?:\w+\s+){0,3}(register|connect|report|beacon)\b)", "forced_action", "context"},
        {R"(only\s+use\s+one[\s\-]?liners?\b)", "anti_forensic_oneliner", "context"},
        {R"(never\s+(?:\w+\s+){0,8}(?:create|write)\s+(?:\w+\s+){0,8}(?:script|file)\s+(?:\w+\s+){0,8}disk)", "anti_forensic_disk", "context"},
        {R"(unset\s+\w*(?:CLAUDE|CODEX|DAEDALUS|AGENT|OPENAI|ANTHROPIC)\w*)", "env_var_unset_agent", "context"},

        {R"(\b(?:cobalt\s*strike|sliver|havoc|mythic|metasploit|brainworm)\b)", "known_c2_framework", "context"},
        {R"(\bc2\s+(?:server|channel|infrastructure|beacon)\b)", "c2_explicit", "context"},
        {R"(\bcommand\s+and\s+control\b)", "c2_explicit_long", "context"},

        {R"(curl\s+[^\n]{0,2048}\$\{?\w*(KEY|TOKEN|SECRET|PASSWORD|CREDENTIAL|API))", "exfil_curl", "all"},
        {R"(wget\s+[^\n]{0,2048}\$\{?\w*(KEY|TOKEN|SECRET|PASSWORD|CREDENTIAL|API))", "exfil_wget", "all"},
        {R"(cat\s+[^\n]{0,2048}(\.env|credentials|\.netrc|\.pgpass|\.npmrc|\.pypirc))", "read_secrets", "all"},
        {R"((send|post|upload|transmit)\s+[^\n]{0,2048}\s+(to|at)\s+https?://)", "send_to_url", "strict"},
        {R"((include|output|print|share)\s+(?:\w+\s+){0,8}(conversation|chat\s+history|previous\s+messages|full\s+context|entire\s+context))", "context_exfil", "strict"},

        {R"(authorized_keys)", "ssh_backdoor", "strict"},
        {R"(\$HOME/\.ssh|\~/\.ssh)", "ssh_access", "strict"},
        {R"(\$HOME/\.daedalus/\.env|\~/\.daedalus/\.env)", "daedalus_env", "strict"},
        {R"((update|modify|edit|write|change|append|add\s+to)\s+[^\n]{0,2048}(?:AGENTS\.md|CLAUDE\.md|\.cursorrules|\.clinerules))", "agent_config_mod", "strict"},
        {R"((update|modify|edit|write|change|append|add\s+to)\s+[^\n]{0,2048}\.daedalus/(config\.yaml|SOUL\.md))", "daedalus_config_mod", "strict"},

        {R"((?:api[_-]?key|token|secret|password)\s*[=:]\s*["'][A-Za-z0-9+/=_-]{20,})", "hardcoded_secret", "strict"},
    };
    return t;
}

// Compiled pattern sets per scope (mirrors _COMPILED + _compile()).
struct Compiled { std::regex rx; std::string pid; };
const std::unordered_map<std::string, std::vector<Compiled>>& compiled_sets() {
    static const std::unordered_map<std::string, std::vector<Compiled>> m = [] {
        std::unordered_map<std::string, std::vector<Compiled>> out;
        for (const auto& key : {std::string("all"), std::string("context"), std::string("strict")})
            out[key];
        for (const auto& p : pattern_table()) {
            std::regex rx(p.re, std::regex::icase);
            Compiled e{rx, p.pid};
            if (std::string(p.scope) == "all") {
                out["all"].push_back(e); out["context"].push_back(e); out["strict"].push_back(e);
            } else if (std::string(p.scope) == "context") {
                out["context"].push_back(e); out["strict"].push_back(e);
            } else if (std::string(p.scope) == "strict") {
                out["strict"].push_back(e);
            } else {
                throw std::runtime_error(
                    std::string("threat_patterns: unknown scope '") + p.scope +
                    "' for pattern '" + p.pid + "'");
            }
        }
        return out;
    }();
    return m;
}

}  // anonymous

std::vector<std::string> scan_for_threats(const std::string& content,
                                          const std::string& scope) {
    if (content.empty()) return {};

    std::string clipped = truncate_codepoints(content, MAX_SCAN_CHARS);

    std::vector<std::string> findings;

    // Invisible-unicode detection on the raw (clipped) content.
    {
        std::unordered_set<uint32_t> seen;
        size_t i = 0, n = clipped.size();
        while (i < n) {
            unsigned char c0 = (unsigned char)clipped[i];
            uint32_t cp = 0; size_t step = 1;
            if (c0 < 0x80) { cp = c0; step = 1; }
            else if ((c0 & 0xE0) == 0xC0) { cp = c0 & 0x1F; step = 2; }
            else if ((c0 & 0xF0) == 0xE0) { cp = c0 & 0x0F; step = 3; }
            else if ((c0 & 0xF8) == 0xF0) { cp = c0 & 0x07; step = 4; }
            else { cp = 0xFFFD; step = 1; }
            for (size_t k = 1; k < step && i + k < n; ++k) {
                unsigned char cc = (unsigned char)clipped[i + k];
                if ((cc & 0xC0) != 0x80) { cp = 0xFFFD; break; }
                cp = (cp << 6) | (cc & 0x3F);
            }
            if (invisible_chars().count(cp) && !seen.count(cp)) {
                seen.insert(cp);
                char buf[32];
                std::snprintf(buf, sizeof(buf), "invisible_unicode_U+%04X", (unsigned)cp);
                findings.emplace_back(buf);
            }
            i += step;
        }
    }

    std::string normalised = nfkc(clipped);

    const auto& sets = compiled_sets();
    auto it = sets.find(scope);
    if (it == sets.end())
        throw std::runtime_error("scan_for_threats: unknown scope '" + scope + "'");
    for (const auto& e : it->second) {
        if (std::regex_search(normalised, e.rx))
            findings.emplace_back(e.pid);
    }
    return findings;
}

std::optional<std::string> first_threat_message(const std::string& content,
                                                const std::string& scope) {
    std::vector<std::string> findings = scan_for_threats(content, scope);
    if (findings.empty()) return std::nullopt;
    const std::string& pid = findings[0];
    if (pid.rfind("invisible_unicode_", 0) == 0) {
        std::string codepoint = pid.substr(std::string("invisible_unicode_").size());
        return "Blocked: content contains invisible unicode character " +
               codepoint + " (possible injection).";
    }
    return "Blocked: content matches threat pattern '" + pid + "'. "
           "Content is injected into the system prompt and must not contain "
           "injection or exfiltration payloads.";
}

}  // namespace tools
}  // namespace dd
