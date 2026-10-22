// src/know/tripwire.cpp — impl of dd/know/tripwire.hpp.
#include "dd/know/tripwire.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

#include "dd/core/yaml.hpp"
#include "dd/tools/registry.hpp"

namespace dd {
namespace know {

namespace {

using core::Json;

// every string a call carries (its command, path, content ...), joined: what a rule is matched against
void strings_of(const Json& j, std::string& out, size_t cap) {
    if (out.size() >= cap) return;
    if (j.type == Json::Str) out += j.str + "\n";
    else if (j.type == Json::Arr)
        for (const auto& x : j.arr)
            if (x) strings_of(*x, out, cap);
    if (j.type == Json::Obj)
        for (const auto& [k, v] : j.obj)
            if (v && k != "intent") strings_of(*v, out, cap);
}

std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

bool truthy(const std::string& v, bool def) {
    std::string l;
    for (char c : trim(v)) l.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (l == "true" || l == "yes" || l == "1") return true;
    if (l == "false" || l == "no" || l == "0") return false;
    return def;
}

}  // namespace

bool Tripwires::parse(const std::string& file_name, const std::string& text, Rule* out, std::string* why) {
    const auto fm = core::parse_frontmatter(text);
    Rule r;
    auto get = [&](const char* k) {
        auto it = fm.find(k);
        return it == fm.end() ? std::string() : core::unquote(it->second);
    };
    r.name = get("name");
    if (r.name.empty()) {   // the file's name without .md
        r.name = file_name;
        if (const size_t dot = r.name.rfind('.'); dot != std::string::npos) r.name.resize(dot);
    }
    r.description = get("description");
    r.condition = get("condition");
    if (r.condition.empty()) {
        if (why) *why = file_name + ": no condition";
        return false;
    }
    const std::string scope = get("scope");
    if (scope.rfind("tool:", 0) == 0) r.tool = trim(scope.substr(5));
    r.once = truthy(get("once"), true);
    if (const std::string gap = get("repeat_gap"); !gap.empty()) r.repeat_gap = std::max(0, std::atoi(gap.c_str()));
    r.text = trim(core::body(text));
    if (r.text.empty()) r.text = r.description;
    if (r.text.empty()) {
        if (why) *why = file_name + ": nothing to say";
        return false;
    }
    *out = std::move(r);
    return true;
}

bool Tripwires::add(Rule r, std::string* why) {
    Compiled c;
    std::string cond = r.condition;
    auto flags = std::regex::ECMAScript | std::regex::optimize;
    if (cond.rfind("(?i)", 0) == 0) {
        flags |= std::regex::icase;
        cond = cond.substr(4);
    }
    try {
        c.re = std::regex(cond, flags);
    } catch (const std::regex_error& e) {
        if (why) *why = "rule " + r.name + ": bad condition (" + e.what() + ")";
        return false;
    }
    c.rule = std::move(r);
    for (auto& existing : rules_)
        if (existing.rule.name == c.rule.name) {
            existing = std::move(c);
            return true;
        }
    rules_.push_back(std::move(c));
    return true;
}

size_t Tripwires::load_dir(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return 0;
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
        if (e.is_regular_file(ec) && e.path().extension() == ".md") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    size_t n = 0;
    for (const auto& f : files) {
        std::ifstream in(f, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        Rule r;
        if (!parse(f.filename().string(), text, &r, nullptr)) continue;
        r.source = f.string();
        if (add(std::move(r))) ++n;
    }
    return n;
}

size_t Tripwires::load_maze(context::Memory& mem) {
    if (!mem.configured()) return 0;
    const auto labels = mem.labels("rule:", 0, 200);
    std::vector<long> ids;
    for (const auto& h : labels) ids.push_back(h.id);
    size_t n = 0;
    for (const auto& h : mem.get(ids)) {
        Rule r;
        if (!parse(h.label.size() > 5 ? h.label.substr(5) : h.label, h.content, &r, nullptr)) continue;
        r.source = "maze";
        if (add(std::move(r))) ++n;
    }
    return n;
}

std::vector<std::string> Tripwires::names() const {
    std::vector<std::string> out;
    for (const auto& c : rules_) out.push_back(c.rule.name);
    return out;
}

std::vector<const Rule*> Tripwires::check(const core::ToolCall& call) {
    std::vector<const Rule*> fired;
    if (rules_.empty()) return fired;
    bool repaired = false;
    const Json args = core::json_parse_args(call.arguments, repaired);
    std::string text;
    strings_of(args, text, 16384);
    if (text.empty()) text = call.arguments;
    const int turn = turn_ ? turn_() : 0;
    for (auto& c : rules_) {
        if (!c.rule.tool.empty() && c.rule.tool != call.name) continue;
        if (c.rule.once && c.spoken && (c.rule.repeat_gap <= 0 || turn - c.spoken_turn < c.rule.repeat_gap)) continue;
        bool hit = false;
        try {
            hit = std::regex_search(text, c.re);
        } catch (...) {
        }
        if (!hit) continue;
        c.spoken = true;
        c.spoken_turn = turn;
        fired.push_back(&c.rule);
    }
    return fired;
}

std::string Tripwires::veto_text(const std::vector<const Rule*>& rules) {
    std::string msg;
    for (const Rule* r : rules)
        msg += (msg.empty() ? "" : "\n") + std::string("[rule ") + r->name + (r->once ? " · told once" : "") + "] " + r->text;
    msg += "\nThis call was not run. Call again once you have taken the rule into account.";
    return tools::tool_error(msg);
}

std::optional<std::string> TripwireGate::veto(const core::ToolCall& call) {
    const auto fired = trip_.check(call);
    if (fired.empty()) return std::nullopt;
    if (on_fire) {
        std::string names;
        for (const Rule* r : fired) names += (names.empty() ? "" : ", ") + r->name;
        on_fire("⛔ rule " + names + " held " + call.name);
    }
    return Tripwires::veto_text(fired);
}

std::vector<std::pair<std::string, std::string>> Tripwires::starter_rules() {
    return {
        {"podman-not-docker.md",
         "---\nname: podman-not-docker\ndescription: podman and quadlet only\ncondition: '(^|[\\s;&|(])(docker|docker-compose)\\b'\nscope: tool:run_terminal\nrepeat_gap: 40\n---\n"
         "This box runs podman and quadlet units, never docker. Use podman (or podman compose); read skill://podman-quadlet-ops before changing a unit.\n"},
        {"no-ai-trailers.md",
         "---\nname: no-ai-trailers\ndescription: no AI trailers in commits\ncondition: '(?i)git\\s+(commit|merge|tag)[\\s\\S]*(co-authored-by|claude-session|generated with)'\nscope: tool:run_terminal\nrepeat_gap: 40\n---\n"
         "No Co-Authored-By, Claude-Session or 'Generated with' lines in commit messages or PR text. Write the message without them.\n"},
        {"no-commit-all-no-verify.md",
         "---\nname: no-commit-all-no-verify\ndescription: stage by name, never skip hooks\ncondition: 'git\\s+commit\\b.*(\\s-a\\b|\\s-[a-zA-Z]*a[a-zA-Z]*\\s|--all\\b|--no-verify|\\s-n\\b)'\nscope: tool:run_terminal\nrepeat_gap: 40\n---\n"
         "Never `git commit -a` and never --no-verify: stage files by name with git add <paths>; if a hook fails, fix what it found.\n"},
        {"no-force-push-main.md",
         "---\nname: no-force-push-main\ndescription: never force-push main\ncondition: '(?i)git\\s+push\\b.*(--force|\\s-f\\b|--force-with-lease).*\\b(main|master)\\b|git\\s+push\\b.*\\b(main|master)\\b.*(--force|\\s-f\\b)'\nscope: tool:run_terminal\nonce: false\n---\n"
         "Never force-push main or master. Push a branch, or make a new commit on top.\n"},
        {"never-touch-live-pods.md",
         "---\nname: never-touch-live-pods\ndescription: live pods and postgres are off limits\ncondition: '(?i)podman\\s+(rm|stop|kill|restart|rmi|system\\s+reset|pod\\s+(rm|stop|kill|restart))\\b|systemctl\\s+(--user\\s+)?(stop|restart|disable|kill)\\s+\\S*(postgres|mazemaker|wonderland|pod)'\nscope: tool:run_terminal\nrepeat_gap: 40\n---\n"
         "These are the live pods and the postgres database. Do not stop, restart, remove or rebuild them while debugging: say what you would change and ask first.\n"},
        {"ask-before-heavy-launches.md",
         "---\nname: ask-before-heavy-launches\ndescription: heavy launches need a yes\ncondition: '(?i)\\b(llama-server|llama-cli|daedalus\\s+doctor\\s+(start|restart|stop)|make\\s+test)\\b'\nscope: tool:run_terminal\nrepeat_gap: 40\n---\n"
         "Model servers, engine restarts and full test suites are heavy on this workstation: ask the operator for a yes first, then run it.\n"},
    };
}

}  // namespace know
}  // namespace dd
