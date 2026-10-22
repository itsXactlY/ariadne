// src/know/signal.cpp — a tool call and its result, boiled down for retrieval.
#include "dd/know/signal.hpp"

#include <algorithm>
#include <cctype>
#include <set>

#include "dd/core/json.hpp"
#include "dd/know/text.hpp"

namespace dd {
namespace know {

namespace {

using core::Json;

unsigned char uc(char c) { return static_cast<unsigned char>(c); }

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(uc(c)));
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

// a path or a file name becomes <path>, numbers become #: "src/a.cpp:312: error" and "src/b.cpp:77: error" are one error
std::string fold_line(const std::string& line) {
    auto word_char = [](char c) { return std::isalnum(uc(c)) || c == '_' || c == '.' || c == '/' || c == '~' || c == '@' || c == '+' || c == '-'; };
    std::string out;
    size_t i = 0;
    while (i < line.size()) {
        if (word_char(line[i])) {
            size_t j = i;
            while (j < line.size() && word_char(line[j])) ++j;
            const std::string w = line.substr(i, j - i);
            const size_t dot = w.rfind('.');
            const bool file = dot != std::string::npos && dot > 0 && dot + 1 < w.size() && w.size() - dot <= 6 &&
                              std::all_of(w.begin() + static_cast<long>(dot) + 1, w.end(), [](char c) { return std::isalnum(uc(c)) != 0; }) &&
                              std::isalpha(uc(w[dot + 1])) != 0;
            if (w.find('/') != std::string::npos && w.size() > 2) out += "<path>";
            else if (file) out += "<path>";
            else out += w;
            i = j;
            continue;
        }
        if (std::isdigit(uc(line[i]))) {
            while (i < line.size() && std::isdigit(uc(line[i]))) ++i;
            out += '#';
            continue;
        }
        out.push_back(line[i++]);
    }
    // numbers inside words ("E0433", "x86") fold too
    std::string folded;
    for (size_t k = 0; k < out.size();) {
        if (std::isdigit(uc(out[k]))) {
            while (k < out.size() && std::isdigit(uc(out[k]))) ++k;
            folded += '#';
        } else {
            folded.push_back(out[k++]);
        }
    }
    return squash(folded);
}

bool error_cue(const std::string& line) {
    static const char* cues[] = {"error", "failed", "failure", "cannot", "can't", "not found", "no such", "permission denied",
                                 "denied", "traceback", "fatal", "undefined reference", "exception", "refused", "timed out",
                                 "unable to", "invalid", "segmentation", "panic", "exit status", "abort", "does not exist"};
    const std::string l = lower(line);
    for (const char* c : cues)
        if (l.find(c) != std::string::npos) return true;
    return false;
}

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        if (const std::string l = squash(text.substr(pos, e - pos)); !l.empty()) out.push_back(l);
        pos = e + 1;
    }
    return out;
}

std::string str_at(const Json& o, const char* key) {
    const Json* v = core::json_get(o, key);
    return v && v->type == Json::Str ? v->str : "";
}

// the programs a shell command runs: pipeline and && / ; segments, wrappers and VAR=x skipped
std::vector<std::string> binaries_of(const std::string& cmd) {
    std::vector<std::string> out;
    std::vector<std::string> segs;
    std::string cur;
    char quote = 0;
    for (size_t i = 0; i < cmd.size(); ++i) {
        const char c = cmd[i];
        if (quote) {
            if (c == quote) quote = 0;
            cur.push_back(c);
        } else if (c == '\'' || c == '"') {
            quote = c;
            cur.push_back(c);
        } else if (c == '|' || c == ';' || c == '\n' || (c == '&' && i + 1 < cmd.size() && cmd[i + 1] == '&')) {
            segs.push_back(cur);
            cur.clear();
            if (c == '&' || (c == '|' && i + 1 < cmd.size() && cmd[i + 1] == '|')) ++i;
        } else {
            cur.push_back(c);
        }
    }
    segs.push_back(cur);
    static const std::set<std::string> wrappers = {"sudo", "env", "time", "nice", "ionice", "nohup", "exec", "command", "stdbuf", "xargs", "watch", "timeout", "setsid"};
    for (const auto& seg : segs) {
        std::vector<std::string> toks;
        std::string t;
        for (char c : seg) {
            if (std::isspace(uc(c))) {
                if (!t.empty()) toks.push_back(t);
                t.clear();
            } else {
                t.push_back(c);
            }
        }
        if (!t.empty()) toks.push_back(t);
        for (size_t i = 0; i < toks.size(); ++i) {
            const std::string& w = toks[i];
            const size_t eq = w.find('=');
            if (eq != std::string::npos && eq > 0 && w.find('/') > eq && std::isalpha(uc(w[0]))) continue;   // VAR=value
            if (w == "{" || w == "(" || w == "!") continue;
            const size_t slash = w.rfind('/');
            const std::string base = lower(slash == std::string::npos ? w : w.substr(slash + 1));
            if (base.empty() || base[0] == '-') continue;
            if (wrappers.count(base)) {
                if (base == "timeout" && i + 1 < toks.size()) ++i;   // its duration
                continue;
            }
            out.push_back(base);
            break;
        }
    }
    return out;
}

void add_path(std::vector<std::string>& paths, std::string p) {
    while (!p.empty() && std::string_view("\"'`(),;").find(p.front()) != std::string_view::npos) p.erase(p.begin());
    while (!p.empty() && std::string_view("\"'`(),;").find(p.back()) != std::string_view::npos) p.pop_back();
    if (p.empty() || p.front() == '-' || p.rfind("http", 0) == 0) return;
    if (std::find(paths.begin(), paths.end(), p) == paths.end()) paths.push_back(std::move(p));
}

bool looks_like_path(const std::string& w) {
    if (w.empty() || w.front() == '-') return false;
    if (w.find('/') != std::string::npos) return true;
    const size_t dot = w.rfind('.');
    return dot != std::string::npos && dot > 0 && w.size() - dot <= 6 && std::isalpha(uc(w[dot + 1]));
}

}  // namespace

std::string error_signature(const std::string& text) {
    const auto lines = lines_of(text);
    std::vector<std::string> picked;
    for (const auto& l : lines) {
        if (!error_cue(l)) continue;
        picked.push_back(fold_line(l));
        if (picked.size() == 2) break;
    }
    if (picked.empty() && !lines.empty()) picked.push_back(fold_line(lines.back()));   // no cue: the tail is usually the error
    std::string sig;
    for (const auto& p : picked) sig += (sig.empty() ? "" : " | ") + p;
    return utf8_head(sig, 160);
}

CallRecord make_record(const core::ToolCall& call, const std::string& result) {
    CallRecord r;
    r.tool = call.name;
    r.args = call.arguments;
    r.bytes = result.size();

    bool repaired = false;
    const Json args = core::json_parse_args(call.arguments, repaired);
    std::string cmd;
    if (args.type == Json::Obj) {
        r.intent = squash(str_at(args, "intent"));
        for (const char* k : {"path", "file", "file_path", "dir", "directory", "cwd", "src", "dest", "target"})
            if (const std::string v = str_at(args, k); !v.empty()) add_path(r.paths, v);
        for (const char* k : {"paths", "files"})
            if (const Json* a = core::json_get(args, k); a && a->type == Json::Arr)
                for (const auto& x : a->arr)
                    if (x && x->type == Json::Str) add_path(r.paths, x->str);
        cmd = str_at(args, "cmd");
        if (cmd.empty()) cmd = str_at(args, "command");
        // the digest: the call without the words the model used to describe it
        Json plain = core::json_obj();
        for (const auto& [k, v] : args.obj)
            if (k != "intent" && v) core::obj_set(plain, k, *v);
        r.digest = call.name + "|" + squash(core::json_dump(plain));
    } else {
        r.digest = call.name + "|" + squash(call.arguments);
    }
    if (!cmd.empty()) {
        r.binaries = binaries_of(cmd);
        std::string t;
        for (char c : cmd + " ") {
            if (std::isspace(uc(c))) {
                if (looks_like_path(t)) add_path(r.paths, t);
                t.clear();
            } else {
                t.push_back(c);
            }
        }
    }

    // the result
    std::string error_text;
    bool json_ok = false;
    try {
        const Json j = core::json_parse(result);
        json_ok = j.type == Json::Obj;
        if (json_ok) {
            if (const Json* e = core::json_get(j, "error"); e) {
                r.failed = true;
                error_text = e->type == Json::Str ? e->str : core::json_dump(*e);
            }
            if (const Json* e = core::json_get(j, "isError"); e && e->type == Json::Bool && e->b) {
                r.failed = true;
                error_text = result;
            }
            if (const Json* x = core::json_get(j, "exit"); x && x->type == Json::Num && x->num != 0) {
                r.failed = true;
                r.exit = static_cast<int>(x->num);
                error_text = str_at(j, "stdout");
            }
            const std::string out = str_at(j, "stdout");
            if (!r.failed) {
                if (core::json_get(j, "stdout") && squash(out).empty()) r.empty = true;
                if (const Json* en = core::json_get(j, "entries"); en && en->type == Json::Arr && en->arr.empty()) r.empty = true;
                if (const Json* ct = core::json_get(j, "content"); ct && ct->type == Json::Str && ct->str.empty()) r.empty = true;
            }
        }
    } catch (...) {
    }
    if (!json_ok && squash(result).empty()) r.empty = true;
    if (r.failed && error_text.rfind("[rule ", 0) == 0) {   // a tripwire's refusal: not run, nothing to look up
        r.refused = true;
        r.failed = false;
    }
    if (r.failed) {
        r.error = error_signature(error_text);
        r.empty = true;   // a failed look found nothing
    }

    static const std::set<std::string> looks = {"ls", "find", "tree", "grep", "rg", "cat", "head", "tail", "fd", "locate", "which", "whereis",
                                                "pwd", "stat", "file", "du", "wc", "less", "more", "ag", "ack", "realpath", "readlink", "type", "test"};
    r.exploring = r.tool == "list_dir" || (r.tool == "read_file" && r.failed) ||
                  (r.tool == "run_terminal" && !r.binaries.empty() && looks.count(r.binaries.front()) > 0);
    return r;
}

}  // namespace know
}  // namespace dd
