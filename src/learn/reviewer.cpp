#include "dd/learn/reviewer.hpp"

#include <regex>

#include "dd/know/text.hpp"

namespace dd {
namespace learn {
namespace {

struct Rule {
    const char* label;
    std::regex re;
};

const std::vector<Rule>& secret_rules() {
    static const std::vector<Rule> r = [] {
        const auto ic = std::regex::ECMAScript | std::regex::icase;
        std::vector<Rule> v;
        v.push_back({"private key", std::regex("-----BEGIN [A-Z ]*PRIVATE KEY-----")});
        v.push_back({"authorization header", std::regex("authorization\\s*[:=]\\s*\\S+", ic)});
        v.push_back({"bearer token", std::regex("bearer\\s+[A-Za-z0-9._~+/=-]{16,}", ic)});
        v.push_back({"github token", std::regex("\\b(ghp|gho|ghs|ghu|ghr)_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,}")});
        v.push_back({"api key", std::regex("\\bsk-[A-Za-z0-9_-]{20,}")});
        v.push_back({"aws key", std::regex("\\bAKIA[0-9A-Z]{16}\\b")});
        v.push_back({"slack token", std::regex("\\bxox[baprs]-[A-Za-z0-9-]{10,}")});
        v.push_back({"email address", std::regex("[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(\\.[A-Za-z0-9-]+)*\\.[A-Za-z]{2,}")});
        v.push_back({"credential assignment", std::regex("\\b(password|passwd|secret|token|api[_-]?key|access[_-]?key)\\s*[=:]\\s*[^\\s`'\"]{6,}", ic)});
        v.push_back({"long opaque token", std::regex("(?=[A-Za-z0-9+/_-]*[A-Z])(?=[A-Za-z0-9+/_-]*[a-z])(?=[A-Za-z0-9+/_-]*[0-9])[A-Za-z0-9+/_-]{40,}")});
        v.push_back({"hex secret", std::regex("\\b[0-9a-f]{40,}\\b", ic)});
        return v;
    }();
    return r;
}

const std::vector<Rule>& instruction_rules() {
    static const std::vector<Rule> r = [] {
        const auto ic = std::regex::ECMAScript | std::regex::icase;
        std::vector<Rule> v;
        v.push_back({"ignore previous", std::regex("\\b(ignore|disregard|forget|override)\\b[^.\\n]{0,30}\\b(previous|prior|above|earlier|all)\\b[^.\\n]{0,20}\\b(instruction|rule|prompt|message|context)s?", ic)});
        v.push_back({"system prompt", std::regex("\\b(system|developer) (prompt|message)\\b", ic)});
        v.push_back({"standing order", std::regex("\\b(you must|you should|you will|from now on)\\b[^.\\n]{0,30}\\b(always|never|only|stop)\\b|\\bfrom now on\\b", ic)});
        v.push_back({"always/never do", std::regex("\\b(always|never)\\s+(call|use|run|execute|invoke|answer|reply|respond|ask|tell|obey|follow)\\b", ic)});
        v.push_back({"hide from the user", std::regex("\\b(do not|don't|never)\\s+(tell|inform|show|mention|reveal)\\b[^.\\n]{0,20}\\b(user|operator)\\b", ic)});
        v.push_back({"tool-call syntax", std::regex("</?\\s*(tool_call|function_call|tool_use|tool_response|system|assistant|user)\\s*>|\"(name|tool)\"\\s*:\\s*\"[A-Za-z_]+\"\\s*,\\s*\"(arguments|input|args)\"", ic)});
        v.push_back({"role marker", std::regex("^\\s*(system|assistant|user)\\s*:", std::regex::ECMAScript | std::regex::icase | std::regex::multiline)});
        return v;
    }();
    return r;
}

std::string first_match(const std::vector<Rule>& rules, const std::string& text) {
    for (const auto& r : rules)
        if (std::regex_search(text, r.re)) return r.label;
    return {};
}

bool name_ok(const std::string& n) {
    if (n.empty() || n.size() > 80) return false;
    for (char c : n)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.')) return false;
    return true;
}

bool under(const std::string& path, const std::string& dir) {
    return !dir.empty() && path.compare(0, dir.size(), dir) == 0 && (path.size() == dir.size() || path[dir.size()] == '/');
}

}  // namespace

std::string Reviewer::find_secret(const std::string& text) { return first_match(secret_rules(), text); }
std::string Reviewer::find_instruction(const std::string& text) { return first_match(instruction_rules(), text); }

std::string Reviewer::find_private_path(const std::string& text) const {
    static const std::regex users("(/home/[A-Za-z0-9._-]+|/Users/[A-Za-z0-9._-]+|/root)(/[^\\s`'\"),]*)?");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), users); it != std::sregex_iterator(); ++it) {
        const std::string p = (*it)[0].str();
        if (under(p, opt_.project_dir)) continue;
        return (*it)[1].str();
    }
    if (!opt_.home_dir.empty()) {
        for (size_t at = 0; (at = text.find(opt_.home_dir, at)) != std::string::npos; at += opt_.home_dir.size()) {
            size_t end = at;
            while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])) && text[end] != '`' && text[end] != '"') ++end;
            if (!under(text.substr(at, end - at), opt_.project_dir)) return opt_.home_dir;
        }
    }
    return {};
}

Verdict Reviewer::review(const Draft& d) const {
    auto no = [](std::string r) { return Verdict{false, std::move(r)}; };
    if (!name_ok(d.name)) return no("size: bad name");
    if (d.category.empty() || d.category.find('/') != std::string::npos || d.category.find("..") != std::string::npos) return no("size: bad category");
    if (d.gist.empty() || d.gist.size() > opt_.max_gist) return no("size: gist length");
    if (d.body.size() < opt_.min_body) return no("size: body too short");
    if (d.body.size() > opt_.max_body) return no("size: body too long");
    if (d.tags.size() > opt_.max_tags) return no("size: too many tags");

    const std::string all = d.name + "\n" + d.gist + "\n" + d.body;
    if (const auto s = find_secret(all); !s.empty()) return no("secret: " + s);
    if (const auto p = find_private_path(all); !p.empty()) return no("private path: " + p);
    if (const auto i = find_instruction(all); !i.empty()) return no("instruction: " + i);

    if (search_) {
        know::Distiller dist;
        const know::Query q = dist.text(d.gist + " " + d.name);
        for (const auto& h : search_(q))
            if (h.card.uri.id != d.name && h.score >= opt_.dup_score && h.matched >= opt_.dup_matched) return no("duplicate of " + h.card.uri.id);
    }
    return Verdict{true, {}};
}

}  // namespace learn
}  // namespace dd
