#include "dd/learn/author.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <set>

#include "dd/core/sha256.hpp"
#include "dd/know/text.hpp"

namespace dd {
namespace learn {
namespace {

// absolute paths shrink to their last two components, the home prefix to ~, runs of space to one
std::string normalise(const std::string& in, const std::string& home, size_t cap) {
    std::string s = in;
    if (!home.empty())
        for (size_t p; (p = s.find(home)) != std::string::npos;) s.replace(p, home.size(), "~");
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        const bool at_word = i == 0 || std::isspace(static_cast<unsigned char>(s[i - 1])) || s[i - 1] == '"' || s[i - 1] == '\'' || s[i - 1] == '=' || s[i - 1] == '(';
        if (at_word && (s[i] == '/' || (s[i] == '~' && i + 1 < s.size() && s[i + 1] == '/'))) {
            size_t j = i;
            while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j])) && s[j] != '"' && s[j] != '\'' && s[j] != ')' && s[j] != ',') ++j;
            const std::string tok = s.substr(i, j - i);
            std::vector<std::string> parts;
            size_t a = 0;
            while (a <= tok.size()) {
                const size_t b = tok.find('/', a);
                const size_t e = b == std::string::npos ? tok.size() : b;
                if (e > a) parts.push_back(tok.substr(a, e - a));
                if (b == std::string::npos) break;
                a = b + 1;
            }
            if (parts.size() > 2) out += "…/" + parts[parts.size() - 2] + "/" + parts.back();
            else out += tok;
            i = j;
            continue;
        }
        const char c = s[i++];
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!out.empty() && out.back() != ' ') out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return know::utf8_head(out, cap);
}

std::string slugify(const std::vector<std::string>& words, size_t max_words) {
    std::string out;
    size_t n = 0;
    for (const auto& w : words) {
        std::string t;
        for (char c : w)
            if (std::isalnum(static_cast<unsigned char>(c))) t.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        if (t.size() < 3 || t.size() > 24) continue;
        if (!out.empty()) out += '-';
        out += t;
        if (++n >= max_words) break;
    }
    return out;
}

std::vector<std::string> binaries_by_use(const Episode& ep) {
    std::map<std::string, int> n;
    for (const auto& s : ep.steps)
        for (const auto& b : s.call.binaries) ++n[b];
    std::vector<std::pair<std::string, int>> v(n.begin(), n.end());
    std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::vector<std::string> out;
    for (const auto& p : v) out.push_back(p.first);
    return out;
}

std::string basename_of(const std::string& p) { return std::filesystem::path(p).filename().string(); }

}  // namespace

std::string category_for(const std::vector<std::string>& bins) {
    static const std::map<std::string, std::string> table = {
        {"podman", "ops"}, {"systemctl", "ops"}, {"journalctl", "ops"}, {"quadlet", "ops"}, {"docker", "ops"},
        {"git", "git"}, {"gh", "git"},
        {"make", "build"}, {"cmake", "build"}, {"gcc", "build"}, {"g++", "build"}, {"clang", "build"}, {"clang++", "build"}, {"ninja", "build"},
        {"cargo", "rust"}, {"rustc", "rust"},
        {"python", "python"}, {"python3", "python"}, {"pip", "python"}, {"uv", "python"}, {"pytest", "python"},
        {"npm", "node"}, {"node", "node"}, {"pnpm", "node"},
        {"ssh", "network"}, {"curl", "network"}, {"wget", "network"},
    };
    for (const auto& b : bins)
        if (auto it = table.find(b); it != table.end()) return it->second;
    return "learned";
}

std::optional<Draft> TemplateAuthor::author(const Detection& d) const {
    const Episode& ep = d.slice;
    if (ep.steps.empty()) return std::nullopt;
    const auto bins = binaries_by_use(ep);

    // the fix: the calls that worked after the first failure (or all that worked), consecutive repeats folded
    std::vector<const Step*> fix;
    size_t first_fail = ep.steps.size();
    for (size_t i = 0; i < ep.steps.size(); ++i)
        if (ep.steps[i].call.failed && !ep.steps[i].call.refused) { first_fail = i; break; }
    for (size_t i = first_fail < ep.steps.size() ? first_fail + 1 : 0; i < ep.steps.size(); ++i) {
        const auto& s = ep.steps[i];
        if (s.call.failed || s.call.refused || s.call.exploring) continue;
        if (!fix.empty() && fix.back()->call.digest == s.call.digest) continue;
        fix.push_back(&s);
    }
    if (fix.size() > 8) fix.erase(fix.begin(), fix.end() - 8);
    if (fix.empty()) return std::nullopt;

    const std::string sig = d.kind == "fixed" || (d.kind == "pinned" && first_fail < ep.steps.size())
        ? normalise(first_fail < ep.steps.size() && d.signature.empty() ? ep.steps[first_fail].call.error : d.signature, home_, 200)
        : std::string();
    const bool is_fix = !sig.empty();

    std::vector<std::string> words = know::terms_of(sig);
    for (const auto& b : bins) words.push_back(b);
    for (const auto* s : fix)
        for (const auto& p : s->call.paths) words.push_back(basename_of(p));
    if (!is_fix) {
        const auto t = know::terms_of(d.signature);
        words.insert(words.begin(), t.begin(), t.end());
    }
    std::vector<std::string> tags;
    std::set<std::string> seen;
    for (const auto& w : know::terms_of([&] { std::string j; for (const auto& x : words) j += x + " "; return j; }())) {
        if (w.size() < 3 || !seen.insert(w).second) continue;
        tags.push_back(w);
        if (tags.size() >= 8) break;
    }

    Draft out;
    out.kind = d.kind;
    out.category = category_for(bins);
    out.episode = ep.digest();
    const std::string prefix = is_fix ? "fix" : (d.kind == "long" ? "flow" : "note");
    const std::string stem = slugify(is_fix ? know::terms_of(sig) : words, 3);
    // the name depends on the problem (signature or topic), not on the fix, so the same problem gets the same skill
    const std::string id = core::sha256_hex(prefix + "|" + (is_fix ? sig : d.signature + "|" + out.category)).substr(0, 6);
    out.name = prefix + "-" + (stem.empty() ? "work" : stem) + "-" + id;
    out.tags = tags;
    out.tags.push_back("learned");

    const std::string via = bins.empty() ? fix.back()->call.tool : bins.front();
    if (is_fix) out.gist = know::utf8_head("Fix for: " + normalise(sig, "", 120) + " — solved with " + via + ".", 200);
    else out.gist = know::utf8_head("Workflow on " + (d.signature.empty() ? via : d.signature) + ": " + normalise(fix.front()->call.intent.empty() ? fix.front()->call.args : fix.front()->call.intent, home_, 100), 200);

    std::string b = "# " + out.name + "\n\n";
    if (is_fix) {
        b += "## Symptom\n\n`" + sig + "`\n\n";
        if (first_fail < ep.steps.size()) b += "Tried first (failed): `" + normalise(ep.steps[first_fail].call.tool + " " + ep.steps[first_fail].call.args, home_, 160) + "`\n\n";
        b += "## Fix\n\n";
    } else {
        b += "## Steps that worked\n\n";
    }
    int n = 0;
    for (const auto* s : fix) {
        b += std::to_string(++n) + ". `" + s->call.tool + "`: " + normalise(s->call.args, home_, 160);
        if (!s->call.intent.empty()) b += " — " + normalise(s->call.intent, home_, 80);
        b += "\n";
    }
    b += "\n_Distilled from observed work; check it still applies before relying on it._\n";
    out.body = b;
    out.digest = core::sha256_hex(out.name + "\n" + out.body).substr(0, 16);
    return out;
}

}  // namespace learn
}  // namespace dd
