// dd::tools shell: the denylist guard and the bounded command runner. A guard, not a sandbox.
#include "dd/tools/shell.hpp"
#include "dd/tools/terminal_hints.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <set>

#include "dd/core/json.hpp"

extern char** environ;

namespace dd {
namespace tools {

using namespace dd::core;

namespace {

JsonSP jsp(Json v) { return std::make_shared<Json>(std::move(v)); }
std::string jerr(const std::string& m) { return json_dump(json_obj({{"error", jsp(json_str(m))}})); }

bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
bool ends_with(const std::string& s, const char* p) {
    const size_t n = std::strlen(p);
    return s.size() >= n && s.compare(s.size() - n, n, p) == 0;
}
std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Verbs whose silence is information: an empty output from them means nothing matched. `cp`, `mv`,
// `sed -i`, `test` and a build legitimately print nothing on success, so they are not here.
bool is_query_command(const std::string& cmd) {
    static const std::set<std::string> verbs = {
        "grep", "egrep", "fgrep", "rg", "ag", "find", "ls", "cat", "head", "tail", "wc",
        "diff", "sort", "uniq", "cut", "md5sum", "sha1sum", "sha256sum", "du", "stat"};
    size_t i = 0;
    while (i < cmd.size() && (cmd[i] == ' ' || cmd[i] == '{' || cmd[i] == '(')) ++i;
    size_t j = i;
    while (j < cmd.size() && cmd[j] != ' ' && cmd[j] != '\n' && cmd[j] != '|' && cmd[j] != ';') ++j;
    std::string first = cmd.substr(i, j - i);
    const size_t slash = first.find_last_of('/');
    if (slash != std::string::npos) first = first.substr(slash + 1);
    return verbs.count(first) != 0;
}

// ---------------- reading a command the way a shell would ----------------

constexpr char kSub = '\x01';   // stands for a substitution inside a word

struct Redir {
    std::string op, target;
};

struct Cmd {
    std::vector<std::string> words;
    std::vector<Redir> redirs;
    std::vector<std::string> subs;       // bodies of $(...) and `...` in this command's words
    std::vector<std::string> procsubs;   // bodies of <(...) and >(...)
    std::string heredoc;
    bool heredoc_expands = false;        // an unquoted delimiter: $(...) in the body runs
    bool piped_from_prev = false;
    size_t pipeline = 0;
};

size_t match_backtick(const std::string& s, size_t i) {   // i: after the opening `; the index of the closing one
    while (i < s.size() && s[i] != '`') i += s[i] == '\\' ? 2 : 1;
    return std::min(i, s.size());
}

// i: the start of the line after the one that held <<. body_end: where the body stops; returns the index after the delimiter line.
size_t heredoc_body(const std::string& s, size_t i, const std::string& delim, bool strip_tabs, size_t* body_end) {
    *body_end = i;
    while (i < s.size()) {
        size_t e = s.find('\n', i);
        if (e == std::string::npos) e = s.size();
        size_t b = i;
        if (strip_tabs)
            while (b < e && s[b] == '\t') ++b;
        const bool end = s.compare(b, e - b, delim) == 0;
        if (end) return e < s.size() ? e + 1 : e;
        i = e < s.size() ? e + 1 : e;
        *body_end = i;
    }
    return i;
}

size_t match_paren(const std::string& s, size_t i);

// i: after the opening ". Returns the index after the closing one.
size_t skip_dquote(const std::string& s, size_t i) {
    while (i < s.size()) {
        const char c = s[i];
        if (c == '\\') i += 2;
        else if (c == '"') return i + 1;
        else if (c == '$' && i + 1 < s.size() && s[i + 1] == '(') i = match_paren(s, i + 2) + 1;
        else if (c == '`') i = match_backtick(s, i + 1) + 1;
        else ++i;
    }
    return s.size();
}

// i: after the opening (. The index of the matching ), quote-, nesting- and heredoc-aware.
size_t match_paren(const std::string& s, size_t i) {
    int depth = 1;
    struct Hd {
        std::string delim;
        bool strip;
    };
    std::vector<Hd> pending;
    while (i < s.size()) {
        const char c = s[i];
        if (c == '\\') {
            i += 2;
        } else if (c == '\'') {
            const size_t e = s.find('\'', i + 1);
            i = e == std::string::npos ? s.size() : e + 1;
        } else if (c == '"') {
            i = skip_dquote(s, i + 1);
        } else if (c == '`') {
            i = match_backtick(s, i + 1) + 1;
        } else if (c == '(') {
            ++depth;
            ++i;
        } else if (c == ')') {
            if (--depth == 0) return i;
            ++i;
        } else if (c == '<' && i + 2 < s.size() && s[i + 1] == '<' && s[i + 2] != '<') {
            size_t j = i + 2;
            const bool strip = s[j] == '-';
            if (strip) ++j;
            while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
            std::string d;
            while (j < s.size() && !std::strchr(" \t\n;|&()<>", s[j])) {
                if (s[j] != '\'' && s[j] != '"' && s[j] != '\\') d += s[j];
                ++j;
            }
            pending.push_back({d, strip});
            i = j;
        } else if (c == '\n' && !pending.empty()) {
            ++i;
            for (const auto& p : pending) {
                size_t be;
                i = heredoc_body(s, i, p.delim, p.strip, &be);
            }
            pending.clear();
        } else {
            ++i;
        }
    }
    return s.size();
}

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s), n_(s.size()) {}

    std::vector<Cmd> run() {
        while (i_ < n_) step();
        end_cmd(false);
        return std::move(out_);
    }

private:
    struct Pending {
        std::string delim;
        bool strip, quoted;
        size_t idx;
    };
    const std::string& s_;
    const size_t n_;
    size_t i_ = 0;
    std::vector<Cmd> out_;
    Cmd cur_;
    std::string w_, pending_op_;
    bool have_ = false, quoted_ = false;
    size_t pipeline_ = 0;
    std::vector<Pending> pend_;

    void flush() {
        if (!have_) return;
        if (!pending_op_.empty()) {
            if (pending_op_ == "<<" || pending_op_ == "<<-") pend_.push_back({w_, pending_op_ == "<<-", quoted_, out_.size()});
            else cur_.redirs.push_back({pending_op_, w_});
            pending_op_.clear();
        } else {
            cur_.words.push_back(w_);
        }
        w_.clear();
        have_ = quoted_ = false;
    }

    void end_cmd(bool pipe_after) {
        flush();
        pending_op_.clear();
        if (!cur_.words.empty() || !cur_.redirs.empty()) out_.push_back(std::move(cur_));
        cur_ = Cmd{};
        cur_.piped_from_prev = pipe_after;
        if (!pipe_after) ++pipeline_;
        cur_.pipeline = pipeline_;
    }

    void heredocs() {   // after a newline: the bodies of the << on the line just ended
        for (const auto& p : pend_) {
            const size_t start = i_;
            size_t be;
            i_ = heredoc_body(s_, i_, p.delim, p.strip, &be);
            if (p.idx < out_.size()) {
                out_[p.idx].heredoc += s_.substr(start, be - start);
                out_[p.idx].heredoc_expands = !p.quoted;
            }
        }
        pend_.clear();
    }

    void ansi_c() {   // at $'
        size_t j = i_ + 2;
        std::string t;
        while (j < n_ && s_[j] != '\'') {
            if (s_[j] != '\\' || j + 1 >= n_) {
                t += s_[j++];
                continue;
            }
            const char e = s_[j + 1];
            j += 2;
            switch (e) {
                case 'n': t += '\n'; break;
                case 't': t += '\t'; break;
                case 'r': t += '\r'; break;
                case 'a': t += '\a'; break;
                case 'b': t += '\b'; break;
                case 'f': t += '\f'; break;
                case 'v': t += '\v'; break;
                case 'e': case 'E': t += '\x1b'; break;
                case '\\': case '\'': case '"': case '?': t += e; break;
                case 'x': {
                    int v = 0, k = 0;
                    while (k < 2 && j < n_ && std::isxdigit(static_cast<unsigned char>(s_[j]))) {
                        v = v * 16 + (std::isdigit(static_cast<unsigned char>(s_[j])) ? s_[j] - '0' : std::tolower(static_cast<unsigned char>(s_[j])) - 'a' + 10);
                        ++j, ++k;
                    }
                    t += static_cast<char>(v);
                    break;
                }
                default:
                    if (e >= '0' && e <= '7') {
                        int v = e - '0', k = 1;
                        while (k < 3 && j < n_ && s_[j] >= '0' && s_[j] <= '7') v = v * 8 + (s_[j++] - '0'), ++k;
                        t += static_cast<char>(v);
                    } else {
                        t += '\\';
                        t += e;
                    }
            }
        }
        i_ = j < n_ ? j + 1 : n_;
        w_ += t;
        have_ = quoted_ = true;
    }

    void dollar(bool in_quotes) {   // at $
        if (i_ + 1 >= n_) {
            w_ += '$';
            have_ = true;
            ++i_;
            return;
        }
        const char d = s_[i_ + 1];
        if (d == '(') {
            const size_t e = match_paren(s_, i_ + 2);
            const std::string body = s_.substr(i_ + 2, e - i_ - 2);
            i_ = e < n_ ? e + 1 : n_;
            have_ = true;
            w_ += kSub;
            const bool arith = body.size() >= 2 && body.front() == '(' && body.back() == ')';   // $(( ... ))
            if (!arith) cur_.subs.push_back(body);
        } else if (d == '{') {
            size_t e = s_.find('}', i_ + 2);
            if (e == std::string::npos) e = n_ - 1;
            w_.append(s_, i_, e - i_ + 1);
            i_ = e + 1;
            have_ = true;
        } else if (!in_quotes && d == '\'') {
            ansi_c();
        } else if (!in_quotes && d == '"') {
            i_ += 2;
            dquote();
        } else {
            w_ += '$';
            have_ = true;
            ++i_;
        }
    }

    void backtick() {
        const size_t e = match_backtick(s_, i_ + 1);
        cur_.subs.push_back(s_.substr(i_ + 1, e - i_ - 1));
        w_ += kSub;
        have_ = true;
        i_ = e < n_ ? e + 1 : n_;
    }

    void dquote() {   // after the opening "
        have_ = quoted_ = true;
        while (i_ < n_ && s_[i_] != '"') {
            const char c = s_[i_];
            if (c == '\\' && i_ + 1 < n_) {
                const char d = s_[i_ + 1];
                if (d == '\n') {
                    i_ += 2;
                } else if (d == '"' || d == '\\' || d == '$' || d == '`') {
                    w_ += d;
                    i_ += 2;
                } else {
                    w_ += c;
                    ++i_;
                }
            } else if (c == '$') {
                dollar(true);
            } else if (c == '`') {
                backtick();
            } else {
                w_ += c;
                ++i_;
            }
        }
        if (i_ < n_) ++i_;
    }

    void redirect() {
        const char c = s_[i_];
        if (have_ && !quoted_ && !w_.empty() && w_.find_first_not_of("0123456789") == std::string::npos) {   // 2> : the fd belongs to the operator
            w_.clear();
            have_ = false;
        } else {
            flush();
        }
        if ((c == '<' || c == '>') && i_ + 1 < n_ && s_[i_ + 1] == '(') {   // <(...) >(...)
            const size_t e = match_paren(s_, i_ + 2);
            const std::string body = s_.substr(i_ + 2, e - i_ - 2);
            cur_.subs.push_back(body);
            cur_.procsubs.push_back(body);
            w_ += kSub;
            have_ = true;
            i_ = e < n_ ? e + 1 : n_;
            return;
        }
        std::string op(1, c);
        ++i_;
        if (c == '&') {
            op = "&>";
            ++i_;   // the >
            if (i_ < n_ && s_[i_] == '>') op += s_[i_++];
        } else if (i_ < n_) {
            const char d = s_[i_];
            if (c == '<' && d == '<') {
                op = "<<";
                ++i_;
                if (i_ < n_ && s_[i_] == '<') op = "<<<", ++i_;
                else if (i_ < n_ && s_[i_] == '-') op = "<<-", ++i_;
            } else if (c == '>' && d == '>') {
                op = ">>", ++i_;
            } else if (c == '>' && d == '|') {
                op = ">|", ++i_;
            } else if ((c == '<' || c == '>') && d == '&') {
                op += '&', ++i_;
            } else if (c == '<' && d == '>') {
                op = "<>", ++i_;
            }
        }
        pending_op_ = op;
    }

    void step() {
        const char c = s_[i_];
        switch (c) {
            case '\\':
                if (i_ + 1 < n_) {
                    if (s_[i_ + 1] != '\n') w_ += s_[i_ + 1], have_ = true;
                    i_ += 2;
                } else {
                    ++i_;
                }
                return;
            case '\'': {
                const size_t e = s_.find('\'', i_ + 1);
                const size_t end = e == std::string::npos ? n_ : e;
                w_.append(s_, i_ + 1, end - i_ - 1);
                have_ = quoted_ = true;
                i_ = end < n_ ? end + 1 : n_;
                return;
            }
            case '"':
                ++i_;
                dquote();
                return;
            case '$':
                dollar(false);
                return;
            case '`':
                backtick();
                return;
            case '#':
                if (!have_) {
                    while (i_ < n_ && s_[i_] != '\n') ++i_;
                    return;
                }
                break;
            case ' ': case '\t': case '\r':
                flush();
                ++i_;
                return;
            case '\n':
                end_cmd(false);
                ++i_;
                heredocs();
                return;
            case ';': case '(': case ')':
                end_cmd(false);
                ++i_;
                return;
            case '&':
                if (i_ + 1 < n_ && s_[i_ + 1] == '>') {
                    redirect();
                } else {
                    end_cmd(false);
                    i_ += i_ + 1 < n_ && s_[i_ + 1] == '&' ? 2 : 1;
                }
                return;
            case '|': {
                const char d = i_ + 1 < n_ ? s_[i_ + 1] : '\0';
                if (d == '|') end_cmd(false), i_ += 2;
                else if (d == '&') end_cmd(true), i_ += 2;
                else end_cmd(true), ++i_;
                return;
            }
            case '<': case '>':
                redirect();
                return;
            default: break;
        }
        w_ += c;
        have_ = true;
        ++i_;
    }
};

// $(...) and `...` bodies inside text that is not parsed as a command (an unquoted heredoc body runs them).
void extract_subs(const std::string& t, std::vector<std::string>* out) {
    for (size_t i = 0; i < t.size();) {
        if (t[i] == '\\') {
            i += 2;
        } else if (t[i] == '$' && i + 1 < t.size() && t[i + 1] == '(') {
            const size_t e = match_paren(t, i + 2);
            out->push_back(t.substr(i + 2, e - i - 2));
            i = e + 1;
        } else if (t[i] == '`') {
            const size_t e = match_backtick(t, i + 1);
            out->push_back(t.substr(i + 1, e - i - 1));
            i = e + 1;
        } else {
            ++i;
        }
    }
}

// ---------------- what a command word means ----------------

std::string base_name(const std::string& w) {
    const size_t s = w.rfind('/');
    return s == std::string::npos ? w : w.substr(s + 1);
}
bool is_flag(const std::string& w) { return w.size() > 1 && w[0] == '-'; }
bool is_assignment(const std::string& w) {
    const size_t eq = w.find('=');
    if (eq == std::string::npos || eq == 0) return false;
    if (!(std::isalpha(static_cast<unsigned char>(w[0])) || w[0] == '_')) return false;
    for (size_t i = 0; i < eq; ++i)
        if (!(std::isalnum(static_cast<unsigned char>(w[i])) || w[i] == '_')) return false;
    return true;
}

// -fr, -R, -Rf ...: a short cluster holding one of `letters` (options end at --)
bool short_flag(const std::vector<std::string>& a, const char* letters) {
    for (const auto& x : a) {
        if (x == "--") break;
        if (x.size() > 1 && x[0] == '-' && x[1] != '-' && x.find_first_of(letters, 1) != std::string::npos) return true;
    }
    return false;
}
bool has_word(const std::vector<std::string>& a, const char* w) { return std::find(a.begin(), a.end(), w) != a.end(); }
std::vector<std::string> operands(const std::vector<std::string>& a) {
    std::vector<std::string> o;
    bool opts = true;
    for (const auto& x : a) {
        if (opts && x == "--") {
            opts = false;
            continue;
        }
        if (opts && is_flag(x)) continue;
        o.push_back(x);
    }
    return o;
}

struct Resolved {
    std::string name;
    std::vector<std::string> args;
    bool via_xargs = false;
    bool harmless = false;   // `command -v x`
};

bool valued_flag(const std::string& wrapper, const std::string& f) {
    static const std::vector<std::pair<std::string, std::set<std::string>>> t = {
        {"nice", {"-n", "--adjustment"}},
        {"ionice", {"-c", "-n", "-p", "-P", "-u", "--class", "--classdata"}},
        {"xargs", {"-n", "-L", "-P", "-I", "-d", "-s", "-a", "-E", "-l", "--max-args", "--max-procs", "--delimiter"}},
        {"timeout", {"-s", "-k", "--signal", "--kill-after"}},
        {"env", {"-u", "-C", "-S", "--unset", "--chdir"}},
        {"watch", {"-n", "--interval"}},
        {"taskset", {"-c"}},
        {"stdbuf", {"-i", "-o", "-e"}},
        {"flock", {"-w", "-E"}},
        {"exec", {"-a"}},
        {"time", {"-f", "-o"}},
    };
    for (const auto& e : t)
        if (e.first == wrapper) return e.second.count(f) > 0;
    return false;
}

const std::set<std::string>& wrappers() {
    static const std::set<std::string> w = {"env", "command", "nohup", "exec", "time", "nice", "ionice", "xargs", "timeout", "setsid",
                                            "stdbuf", "builtin", "watch", "taskset", "flock", "chrt", "fakeroot", "busybox"};
    return w;
}

Resolved resolve(const std::vector<std::string>& w) {
    static const std::set<std::string> kIgnore = {"for", "select", "case", "function", "coproc", "fi", "done", "esac", "in"};
    static const std::set<std::string> kPrefix = {"!", "{", "}", "if", "then", "else", "elif", "while", "until", "do"};
    Resolved r;
    size_t i = 0;
    for (;;) {
        while (i < w.size() && (kPrefix.count(w[i]) || is_assignment(w[i]))) ++i;
        if (i >= w.size() || kIgnore.count(w[i])) return r;
        const std::string b = base_name(w[i]);
        if (!wrappers().count(b)) break;
        if (b == "command" && i + 1 < w.size() && (w[i + 1] == "-v" || w[i + 1] == "-V")) {
            r.harmless = true;
            return r;
        }
        if (b == "xargs") r.via_xargs = true;
        int positional = b == "timeout" || b == "flock" || b == "chrt" || b == "taskset" ? 1 : 0;
        size_t j = i + 1;
        while (j < w.size()) {
            const std::string& a = w[j];
            if (a == "--") {
                ++j;
                break;
            }
            if (is_flag(a)) {
                ++j;
                if (b == "taskset" && a == "-c") positional = 0;
                if (valued_flag(b, a) && j < w.size()) ++j;
                continue;
            }
            if (b == "env" && is_assignment(a)) {
                ++j;
                continue;
            }
            if (positional > 0) {
                --positional;
                ++j;
                continue;
            }
            break;
        }
        i = j;
    }
    r.name = base_name(w[i]);
    r.args.assign(w.begin() + static_cast<long>(i) + 1, w.end());
    return r;
}

bool is_shell(const std::string& n) {
    static const std::set<std::string> s = {"sh", "bash", "zsh", "dash", "ksh", "ash", "fish", "csh", "tcsh"};
    return s.count(n) > 0;
}
bool is_scripting(const std::string& n) {
    static const std::set<std::string> s = {"python", "python2", "python3", "perl", "ruby", "node", "php", "lua"};
    return s.count(n) > 0;
}
// a shell or interpreter that would take its program from stdin
bool reads_code_from_stdin(const Resolved& r) {
    if (is_shell(r.name)) {
        if (has_word(r.args, "-s") || short_flag(r.args, "s")) return true;
        for (const auto& a : r.args) {
            if (a == "--") break;
            if (short_flag({a}, "c")) return false;   // -c <string>
        }
        return operands(r.args).empty() || operands(r.args)[0] == "-";
    }
    if (is_scripting(r.name)) {
        const auto o = operands(r.args);
        for (const auto& a : r.args)
            if (is_flag(a) && a != "-") return false;   // -c / -e / -m: the code or module is on the command line
        return o.empty() || o[0] == "-";
    }
    return false;
}
bool is_downloader(const Resolved& r) {
    static const std::set<std::string> s = {"curl", "wget", "fetch", "aria2c", "nc", "ncat", "netcat", "socat", "ftp", "tftp"};
    return s.count(r.name) > 0;
}
bool is_decoder(const Resolved& r) {
    if (r.name == "base64") return short_flag(r.args, "dD") || has_word(r.args, "--decode");
    if (r.name == "xxd") return short_flag(r.args, "r");
    if (r.name == "uudecode") return true;
    if (r.name == "openssl") return (has_word(r.args, "enc") || has_word(r.args, "base64")) && short_flag(r.args, "dD");
    return false;
}

// ---------------- paths that matter ----------------

// ~, $HOME, ${HOME}, the real home, /home/<user>, /Users/<user>, /root -> <HOME>; one slash style; no trailing slash
std::string home_norm(std::string w, const std::string& home) {
    auto sub = [&](const std::string& prefix) {
        if (w.compare(0, prefix.size(), prefix) != 0) return false;
        if (w.size() > prefix.size() && w[prefix.size()] != '/') return false;
        w = "<HOME>" + w.substr(prefix.size());
        return true;
    };
    if (w == "~" || starts_with(w, "~/")) w = "<HOME>" + w.substr(1);
    else if (!sub("$HOME") && !sub("${HOME}") && !(!home.empty() && home != "/" && sub(home)) && !sub("/root")) {
        for (const char* base : {"/home/", "/Users/"}) {
            if (!starts_with(w, base)) continue;
            const size_t slash = w.find('/', std::strlen(base));
            w = "<HOME>" + (slash == std::string::npos ? std::string() : w.substr(slash));
            break;
        }
    }
    std::string o;   // collapse // and /./
    for (size_t i = 0; i < w.size(); ++i) {
        if (w[i] == '/' && !o.empty() && o.back() == '/') continue;
        if (w[i] == '/' && i + 2 < w.size() + 1 && w.compare(i, 3, "/./") == 0) continue;
        o += w[i];
    }
    while (o.size() > 1 && o.back() == '/') o.pop_back();
    return o;
}

bool is_broad(const std::string& word, const std::string& home, bool include_dot) {
    if (word.find(kSub) != std::string::npos || word.empty()) return false;
    if (word == "/" || word == "/*") return true;
    if (word.find_first_not_of("*.?/[]") == std::string::npos) {   // only glob characters and dots
        if (word.find_first_of("*?") != std::string::npos) return true;
        if (include_dot) {
            size_t parts = 0;
            for (size_t i = 0; i < word.size(); ++i)
                if (word[i] != '/') ++parts;
            return parts > 0;   // ., .., ./, ../.. : the working directory or above
        }
        return false;
    }
    std::string h = home_norm(word, home);
    for (const char* suffix : {"/*", "/.*", "/."})
        if (ends_with(h, suffix)) {
            h.resize(h.size() - std::strlen(suffix));
            break;
        }
    while (h.size() > 1 && h.back() == '/') h.pop_back();
    if (h == "<HOME>" || h == "/") return true;
    static const std::set<std::string> top = {"/etc", "/usr", "/var", "/boot", "/bin", "/sbin", "/lib", "/lib32", "/lib64", "/opt", "/dev", "/sys",
                                              "/proc", "/srv", "/mnt", "/media", "/run", "/tmp", "/home", "/snap", "/root"};
    return top.count(h) > 0;
}

bool secret_exact(const std::string& n) {   // n is home_norm'd
    const std::string base = base_name(n);
    static const std::set<std::string> keys = {"id_rsa", "id_ed25519", "id_ecdsa", "id_dsa", "id_ed25519_sk", "id_ecdsa_sk"};
    if (keys.count(base)) return true;
    if (starts_with(n, "<HOME>/.ssh/")) {
        if (ends_with(base, ".pub") || base == "known_hosts" || base == "config" || base == "authorized_keys") return false;
        return true;
    }
    if (n == "<HOME>/.gnupg" || starts_with(n, "<HOME>/.gnupg/")) return true;
    if (n == "/etc/shadow" || n == "/etc/gshadow" || n == "/etc/sudoers" || starts_with(n, "/etc/sudoers.d/")) return true;
    if (starts_with(n, "/etc/ssh/ssh_host_") && !ends_with(n, ".pub")) return true;
    static const std::set<std::string> homefiles = {"<HOME>/.aws/credentials", "<HOME>/.netrc", "<HOME>/.env", "<HOME>/.git-credentials",
                                                    "<HOME>/.docker/config.json", "<HOME>/.kube/config", "<HOME>/.config/gh/hosts.yml",
                                                    "<HOME>/.pypirc", "<HOME>/.npmrc", "<HOME>/.daedalus/.env"};
    return homefiles.count(n) > 0;
}

// a word that names a secret file, as itself, as key=path, as @path (curl), or inside code text
bool secret_word(const std::string& w, const std::string& home) {
    std::vector<std::string> cand = {w};
    if (const size_t eq = w.rfind('='); eq != std::string::npos) cand.push_back(w.substr(eq + 1));
    if (const size_t at = w.rfind('@'); at != std::string::npos) cand.push_back(w.substr(at + 1));
    for (const auto& c : cand)
        if (secret_exact(home_norm(c, home))) return true;
    if (w.find_first_of(" \t;()\"'") != std::string::npos)   // a snippet of code or a command string
        for (const char* needle : {".ssh/id_", ".gnupg", "/etc/shadow", ".aws/credentials", "/.netrc", ".git-credentials"})
            if (w.find(needle) != std::string::npos) return true;
    return false;
}

bool rc_file(const std::string& word, const std::string& home) {
    const std::string n = home_norm(word, home);
    const std::string base = base_name(n);
    static const std::set<std::string> rcs = {".bashrc", ".bash_profile", ".bash_login", ".bash_logout", ".profile", ".zshrc", ".zprofile", ".zshenv", ".zlogin"};
    if (rcs.count(base) && (n.find('/') == std::string::npos || starts_with(n, "<HOME>/"))) return true;
    if (n == "<HOME>/.config/fish/config.fish") return true;
    if (base == "authorized_keys" || base == "authorized_keys2") return true;
    return n == "/etc/profile" || n == "/etc/bash.bashrc" || n == "/etc/environment" || starts_with(n, "/etc/profile.d/");
}

bool ssh_or_gnupg_dir(const std::string& word, const std::string& home) {
    std::string n = home_norm(word, home);
    for (const char* suffix : {"/*", "/.*"})
        if (ends_with(n, suffix)) n.resize(n.size() - std::strlen(suffix));
    return n == "<HOME>/.ssh" || n == "<HOME>/.gnupg";
}

// ---------------- the guard ----------------

class Guard {
public:
    explicit Guard(const ShellPolicy& p) : extra_(p.extra_deny.begin(), p.extra_deny.end()) {
        if (const char* h = std::getenv("HOME")) home_ = h;
    }

    std::string check(const std::string& s, int depth) {
        if (depth > 8) return "command nests too deep to check";
        const std::vector<Cmd> cmds = Parser(s).run();
        std::vector<Resolved> pipe;   // the commands of the current pipeline before this one
        size_t pipeline = static_cast<size_t>(-1);
        for (const Cmd& c : cmds) {
            if (++budget_ > 4000) return "command too complex to check";
            if (c.pipeline != pipeline) {
                pipeline = c.pipeline;
                pipe.clear();
            }
            const Resolved r = resolve(c.words);
            if (std::string why = one(c, r, pipe, depth); !why.empty()) return why;
            pipe.push_back(r);
        }
        return "";
    }

private:
    std::set<std::string> extra_;
    std::string home_;
    int budget_ = 0;

    bool body_runs(const std::string& body, bool (*pred)(const Resolved&)) {
        for (const Cmd& c : Parser(body).run())
            if (pred(resolve(c.words))) return true;
        return false;
    }

    std::string git_rule(const std::vector<std::string>& a) {
        size_t i = 0;
        for (; i < a.size(); ++i) {
            if (a[i] == "-C" || a[i] == "--git-dir" || a[i] == "--work-tree" || a[i] == "--namespace") {
                ++i;
                continue;
            }
            if (a[i] == "-c") {
                if (i + 1 < a.size() && upper(a[i + 1]).rfind("CORE.HOOKSPATH", 0) == 0) return "git hooks disabled via core.hooksPath";
                ++i;
                continue;
            }
            if (is_flag(a[i])) {
                if (upper(a[i]).rfind("-CCORE.HOOKSPATH", 0) == 0) return "git hooks disabled via core.hooksPath";
                continue;
            }
            break;
        }
        if (i >= a.size()) return "";
        const std::string sub = a[i];
        const std::vector<std::string> rest(a.begin() + static_cast<long>(i) + 1, a.end());
        if (has_word(rest, "--no-verify")) return "git --no-verify (skips the hooks)";
        if (sub == "commit" && short_flag(rest, "n")) return "git commit -n is --no-verify (skips the hooks)";
        if (sub == "push") {
            for (const auto& r : rest) {
                if (starts_with(r, "--force")) return "git force-push";
                if (r == "--mirror") return "git push --mirror (rewrites the remote)";
                if (r.size() > 1 && r[0] == '+') return "git push +refspec forces the update";
            }
            if (short_flag(rest, "f")) return "git force-push";
        }
        if (sub == "reset" && has_word(rest, "--hard")) return "git reset --hard destroys uncommitted work: commit or stash first, or run it yourself";
        if (sub == "clean" && (short_flag(rest, "f") || has_word(rest, "--force")) && !short_flag(rest, "n") && !has_word(rest, "--dry-run"))
            return "git clean -f deletes untracked files for good: destroys uncommitted work";
        if (sub == "checkout" || sub == "restore" || sub == "switch") {
            const bool whole_tree = has_word(rest, ".") || has_word(rest, ":/");
            if (sub == "checkout" && (short_flag(rest, "f") || has_word(rest, "--force"))) return "git checkout -f destroys uncommitted work: commit or stash first, or run it yourself";
            if (sub == "switch" && (short_flag(rest, "f") || has_word(rest, "--discard-changes"))) return "git switch --discard-changes destroys uncommitted work";
            if (whole_tree) {
                const bool staged_only = sub == "restore" && has_word(rest, "--staged") && !has_word(rest, "--worktree") && !short_flag(rest, "W");
                if (!staged_only) return "git " + sub + " . discards uncommitted work: commit or stash first, or run it yourself";
            }
        }
        return "";
    }

    std::string one(const Cmd& c, const Resolved& r, const std::vector<Resolved>& pipe, int depth) {
        for (const auto& sub : c.subs)
            if (std::string why = check(sub, depth + 1); !why.empty()) return why;
        if (c.heredoc_expands) {
            std::vector<std::string> subs;
            extract_subs(c.heredoc, &subs);
            for (const auto& sub : subs)
                if (std::string why = check(sub, depth + 1); !why.empty()) return why;
        }
        for (const Redir& rd : c.redirs) {
            const bool dup = (rd.op == ">&" || rd.op == "<&") && (rd.target == "-" || rd.target.find_first_not_of("0123456789") == std::string::npos);
            if (dup) continue;
            if (rd.op == "<" || rd.op == "<>") {
                if (secret_word(rd.target, home_)) return "reading private key or secret files (~/.ssh/id_*, ~/.gnupg, /etc/shadow, ...)";
            } else if (rd.op != "<<<") {
                if (rc_file(rd.target, home_)) return "writing a shell rc file or authorized_keys";
            }
        }
        if (r.name.empty() || r.harmless) return "";

        const std::string& n = r.name;
        const auto& a = r.args;
        static const std::set<std::string> kDeny = {"sudo", "su", "doas", "pkexec", "run0", "sudoedit", "mkswap", "fdisk", "sfdisk", "parted", "wipefs",
                                                    "shred", "dd", "shutdown", "reboot", "poweroff", "halt", "init", "telinit", "killall5", "passwd",
                                                    "chpasswd", "visudo", "useradd", "userdel", "usermod", "docker"};
        if (kDeny.count(n) || extra_.count(n) || starts_with(n, "mkfs")) return "command '" + n + "' is blocked";

        // programs that run text as code
        const bool shell = is_shell(n);
        bool dash_c = false;
        std::string inner;
        if (shell) {
            for (size_t k = 0; k < a.size(); ++k) {
                if (a[k] == "--") break;
                if (is_flag(a[k]) && a[k][1] != '-' && a[k].find('c') != std::string::npos) {
                    dash_c = true;
                    if (k + 1 < a.size()) inner = a[k + 1];
                    break;
                }
            }
            if (dash_c) {
                if (std::string why = check(inner, depth + 1); !why.empty()) return why;
            }
        }
        const bool evals = n == "eval" || n == "source" || n == ".";
        if (n == "eval") {
            std::string joined;
            for (const auto& w : a) joined += (joined.empty() ? "" : " ") + w;
            if (std::string why = check(joined, depth + 1); !why.empty()) return why;
        }
        if (shell || evals) {
            if (!c.heredoc.empty() && (shell || n == "source" || n == ".")) {
                if (std::string why = check(c.heredoc, depth + 1); !why.empty()) return why;
            }
            for (const auto& rd : c.redirs)
                if (rd.op == "<<<")
                    if (std::string why = check(rd.target, depth + 1); !why.empty()) return why;
            const bool code_from_subs = evals || dash_c;
            auto dirty = [](const Resolved& x) { return is_downloader(x) || is_decoder(x); };
            for (const auto& body : c.procsubs)
                if (body_runs(body, +dirty)) return "evaluating downloaded or decoded text";
            if (code_from_subs)
                for (const auto& body : c.subs)
                    if (body_runs(body, +dirty)) return "evaluating downloaded or decoded text";
        }
        if (c.piped_from_prev && reads_code_from_stdin(r)) {
            for (const Resolved& p : pipe) {
                if (is_downloader(p)) return "piping a download into a shell";
                if (is_decoder(p)) return "piping decoded data into a shell";
            }
        }

        const auto ops = operands(a);

        // recursive deletes and permission changes
        if (n == "rm") {
            const bool rec = short_flag(a, "rR") || has_word(a, "--recursive");
            if (rec) {
                if (r.via_xargs) return "recursive rm through xargs (the targets are unknown)";
                for (const auto& o : ops)
                    if (is_broad(o, home_, true)) return "recursive rm on a broad path";
            }
        }
        if (n == "chmod" || n == "chown" || n == "chgrp") {
            if (short_flag(a, "R") || has_word(a, "--recursive")) {
                if (r.via_xargs) return "recursive " + n + " through xargs (the targets are unknown)";
                for (size_t k = 1; k < ops.size(); ++k)   // ops[0] is the mode or the owner
                    if (is_broad(ops[k], home_, true)) return "recursive " + n + " on a broad path";
            }
        }
        if (n == "find") {
            std::vector<std::string> paths;
            size_t k = 0;
            for (; k < a.size() && !(is_flag(a[k]) || a[k] == "(" || a[k] == "!"); ++k) paths.push_back(a[k]);
            static const std::set<std::string> filters = {"-name", "-iname", "-path", "-ipath", "-regex", "-iregex", "-type", "-mtime", "-mmin", "-atime",
                                                          "-ctime", "-newer", "-size", "-user", "-group", "-perm", "-empty", "-links", "-samefile", "-lname"};
            bool destroys = false, filtered = false;
            static const std::set<std::string> killers = {"rm", "shred", "unlink", "rmdir", "mv", "truncate"};
            for (size_t m = k; m < a.size(); ++m) {
                if (a[m] == "-delete") destroys = true;
                if ((a[m] == "-exec" || a[m] == "-execdir" || a[m] == "-ok" || a[m] == "-okdir") && m + 1 < a.size() && killers.count(base_name(a[m + 1]))) destroys = true;
                if (filters.count(a[m])) filtered = true;
            }
            if (destroys) {
                if (paths.empty()) paths.push_back(".");
                for (const auto& p : paths) {
                    if (is_broad(p, home_, false)) return "find deleting from a broad path";
                    if (!filtered && is_broad(p, home_, true)) return "find deleting everything under " + p;
                }
            }
        }

        // private files, rc files
        static const std::set<std::string> readers = {"cat", "tac", "less", "more", "most", "head", "tail", "nl", "od", "xxd", "hexdump", "strings", "base64",
                                                      "cp", "mv", "rsync", "scp", "tar", "zip", "gzip", "bzip2", "xz", "7z", "7za", "grep", "egrep", "fgrep",
                                                      "rg", "ag", "awk", "gawk", "sed", "cut", "sort", "uniq", "tee", "curl", "wget", "nc", "ncat", "netcat",
                                                      "socat", "openssl", "python", "python2", "python3", "perl", "ruby", "node", "php", "lua", "source", ".",
                                                      "paste", "fold", "diff", "cmp", "vim", "vi", "nano", "emacs", "bat", "xclip", "wl-copy"};
        static const std::set<std::string> writers = {"tee", "cp", "mv", "ln", "install", "rm", "truncate", "rsync", "patch", "dd"};
        const bool sed_inplace = (n == "sed" || n == "perl") && short_flag(a, "i");
        for (const auto& w : a) {
            if (readers.count(n) && secret_word(w, home_)) return "reading private key or secret files (~/.ssh/id_*, ~/.gnupg, /etc/shadow, ...)";
            if ((writers.count(n) || sed_inplace) && !is_flag(w) && rc_file(w, home_)) return "writing a shell rc file or authorized_keys";
        }
        if (n == "rm" || n == "mv" || n == "cp" || n == "chmod" || n == "chown" || n == "chgrp" || n == "ln" || n == "rsync" || n == "truncate")
            for (const auto& o : ops)
                if (ssh_or_gnupg_dir(o, home_)) return "touching the ~/.ssh or ~/.gnupg directory";

        // the rest
        if (n == "git") {
            if (std::string why = git_rule(a); !why.empty()) return why;
        }
        if (n == "kill" || n == "pkill")
            for (const auto& x : a)
                if (x == "-1" || x == "1") return "signalling pid 1 / all processes";
        if (n == "systemctl")
            for (const auto& x : a)
                if (x == "poweroff" || x == "reboot" || x == "halt" || x == "suspend" || x == "hibernate" || x == "kexec") return "systemctl " + x;
        if (n == "podman" && a.size() >= 2 && a[0] == "system" && a[1] == "reset") return "podman system reset";
        return "";
    }
};

// ---------------- running a command ----------------

constexpr int kGroups = 64;
std::atomic<pid_t> g_groups[kGroups];   // process groups of commands that may still be running; 0 = free

bool group_alive(pid_t g) { return ::killpg(g, 0) == 0 || errno == EPERM; }

void prune_groups() {
    for (auto& s : g_groups)
        if (const pid_t g = s.load(); g > 0 && !group_alive(g)) s.compare_exchange_strong(const_cast<pid_t&>(g), 0);
}

void register_group(pid_t g) {
    for (int pass = 0; pass < 2; ++pass) {
        for (auto& s : g_groups) {
            pid_t zero = 0;
            if (s.compare_exchange_strong(zero, g)) return;
        }
        prune_groups();
    }
}

void forget_group(pid_t g) {
    for (auto& s : g_groups) {
        pid_t cur = g;
        if (s.compare_exchange_strong(cur, 0)) return;
    }
}

bool secret_env_name(const std::string& name) {
    const std::string u = upper(name);
    return u == "DD_API_KEY" || u == "API_KEY" || ends_with(u, "_API_KEY") || u == "TOKEN" || ends_with(u, "_TOKEN") ||
           u.find("SECRET") != std::string::npos || u.find("PASSWORD") != std::string::npos || u.find("PASSWD") != std::string::npos;
}

void trim_partial_utf8(std::string& s) {
    size_t i = s.size();
    int back = 0;
    while (i > 0 && back < 4 && (static_cast<unsigned char>(s[i - 1]) & 0xC0) == 0x80) --i, ++back;
    if (i == 0) return;
    const unsigned char lead = static_cast<unsigned char>(s[i - 1]);
    const size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (need > 1 && s.size() - (i - 1) < need) s.resize(i - 1);
}

using Clock = std::chrono::steady_clock;

void kill_group(pid_t g, int sig) { ::killpg(g, sig); }

}  // namespace

std::string shell_danger(const std::string& cmd, const ShellPolicy& policy) {
    for (const char* bad : {":(){", "> /dev/sd", ">/dev/sd", "> /dev/nvme", ">/dev/nvme", "of=/dev/", "--no-preserve-root"})
        if (cmd.find(bad) != std::string::npos) return std::string("forbidden pattern '") + bad + "'";
    return Guard(policy).check(cmd, 0);
}

#ifndef __GLIBC__
// Not glibc (bionic on Android, musl on Alpine): neither has posix_spawn_file_actions_addclosefrom_np,
// a glibc 2.34 extension. So the child takes posix_spawn's steps by hand -- a new session, default
// signals and an empty mask, stdin from /dev/null, the pipe on 1 and 2, every other descriptor closed --
// and calls nothing but async-signal-safe functions between fork and exec. Returns 0 or an errno, as
// posix_spawn does.
static int spawn_sh_forked(const char* sh, pid_t* pid, const char* const* argv, char* const* envp, int out) {
    const long open_max = ::sysconf(_SC_OPEN_MAX);
    const int top = open_max > 0 ? static_cast<int>(std::min<long>(open_max, 65536)) : 1024;
    const pid_t p = ::fork();
    if (p < 0) return errno;
    if (p == 0) {
        ::setsid();
        struct sigaction dfl {};
        dfl.sa_handler = SIG_DFL;
        for (int s = 1; s < NSIG; ++s) ::sigaction(s, &dfl, nullptr);   // SIGKILL/SIGSTOP refuse; harmless
        sigset_t none;
        sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);
        const int dn = ::open("/dev/null", O_RDONLY);
        if (dn > 0) {
            ::dup2(dn, 0);
            ::close(dn);
        }
        ::dup2(out, 1);
        ::dup2(out, 2);
        for (int fd = 3; fd < top; ++fd) ::close(fd);
        ::execve(sh, const_cast<char* const*>(argv), envp);
        ::_exit(127);
    }
    *pid = p;
    return 0;
}
#endif

std::string run_shell(const std::string& cmd, const ShellOptions& opt, const ShellPolicy& policy) {
    if (cmd.empty()) return jerr("cmd required");
    if (std::string why = shell_danger(cmd, policy); !why.empty()) return jerr("refused: " + why);
    if (opt.cancel && opt.cancel->load(std::memory_order_relaxed)) return jerr("interrupted");
    prune_groups();

    // a group: the child never reads the tty the TUI holds in raw mode, and one trailing comment cannot eat the brace
    const std::string full = "{ " + cmd + "\n} </dev/null 2>&1";
    std::vector<std::string> env;
    for (char** e = environ; e && *e; ++e) {
        const std::string kv = *e;
        const size_t eq = kv.find('=');
        if (eq != std::string::npos && secret_env_name(kv.substr(0, eq))) continue;
        // Message wording is what the failure hints match on. In a non-English locale bash prints
        // "Kommando nicht gefunden" and every hint silently misses, so messages are pinned to C.
        // Only messages: LC_ALL would also change collation and number formatting in command output.
        if (eq != std::string::npos && kv.substr(0, eq) == "LC_MESSAGES") continue;
        env.push_back(kv);
    }
    env.push_back("LC_MESSAGES=C");
    std::vector<char*> envp;
    for (auto& s : env) envp.push_back(s.data());
    envp.push_back(nullptr);

    int fds[2];
    if (::pipe2(fds, O_CLOEXEC) != 0) return jerr(std::string("pipe failed: ") + std::strerror(errno));
    const char* argv[] = {"sh", "-c", full.c_str(), nullptr};
    pid_t pid = 0;
#ifndef __GLIBC__
#ifdef __ANDROID__
    constexpr const char* kSh = "/system/bin/sh";   // Android has no /bin
#else
    constexpr const char* kSh = "/bin/sh";
#endif
    const int rc = spawn_sh_forked(kSh, &pid, argv, envp.data(), fds[1]);
#else
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    posix_spawn_file_actions_addclosefrom_np(&fa, 3);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &all);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    const int rc = ::posix_spawn(&pid, "/bin/sh", &fa, &at, const_cast<char* const*>(argv), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
#endif
    ::close(fds[1]);
    if (rc != 0) {
        ::close(fds[0]);
        return jerr(std::string("spawn failed: ") + std::strerror(rc));
    }
    register_group(pid);

    const auto t0 = Clock::now();
    const size_t keep = opt.max_output;
    std::string head;
    size_t total = 0;
    bool eof = false, leader_done = false, background = false;
    int status = 0;
    Clock::time_point leader_at{};
    std::string error;
    char buf[1 << 16];
    for (;;) {
        if (opt.cancel && opt.cancel->load(std::memory_order_relaxed)) {
            error = "interrupted";
            break;
        }
        const auto now = Clock::now();
        if (opt.timeout_ms > 0 && now - t0 >= std::chrono::milliseconds(opt.timeout_ms)) {
            error = "timed out after " + std::to_string(opt.timeout_ms) + " ms";
            break;
        }
        if (!eof) {
            pollfd p{fds[0], POLLIN, 0};
            const int pr = ::poll(&p, 1, leader_done ? 10 : 50);
            if (pr > 0) {
                const ssize_t n = ::read(fds[0], buf, sizeof buf);
                if (n > 0) {
                    total += static_cast<size_t>(n);
                    if (head.size() < keep) head.append(buf, std::min(static_cast<size_t>(n), keep - head.size()));
                } else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
                    eof = true;
                }
            }
        } else if (!leader_done) {
            ::poll(nullptr, 0, 5);
        }
        if (!leader_done && ::waitpid(pid, &status, WNOHANG) == pid) {
            leader_done = true;
            leader_at = now;
        }
        if (eof && leader_done) break;
        if (leader_done && !eof && now - leader_at >= std::chrono::milliseconds(300)) {   // a background child still holds the pipe
            background = true;
            break;
        }
    }

    if (!error.empty()) {   // the whole group goes: SIGTERM, then SIGKILL after two seconds
        kill_group(pid, SIGTERM);
        const auto kill_at = Clock::now() + std::chrono::seconds(2);
        while (Clock::now() < kill_at && (!leader_done || group_alive(pid))) {
            pollfd p{fds[0], POLLIN, 0};
            if (!eof && ::poll(&p, 1, 20) > 0) {   // keep the pipe empty so a blocked writer can exit
                const ssize_t n = ::read(fds[0], buf, sizeof buf);
                if (n > 0) {
                    total += static_cast<size_t>(n);
                    if (head.size() < keep) head.append(buf, std::min(static_cast<size_t>(n), keep - head.size()));
                } else if (n == 0) {
                    eof = true;
                }
            } else if (eof) {
                ::poll(nullptr, 0, 20);
            }
            if (!leader_done && ::waitpid(pid, &status, WNOHANG) == pid) leader_done = true;
        }
        kill_group(pid, SIGKILL);
        if (!leader_done) ::waitpid(pid, &status, 0);
        leader_done = true;
    }
    ::close(fds[0]);
    if (!background && !group_alive(pid)) forget_group(pid);

    const bool truncated = total > keep;
    const size_t dropped = truncated ? total - keep : 0;
    trim_partial_utf8(head);
    if (truncated)
        head += "...[truncated: " + std::to_string(dropped) + " of " + std::to_string(total) + " bytes dropped]";
    Json out = json_obj({{"cmd", jsp(json_str(cmd))}});
    if (!error.empty()) {
        obj_set(out, "error", json_str(error));
    } else if (WIFEXITED(status)) {
        obj_set(out, "exit", json_num(static_cast<double>(WEXITSTATUS(status))));
    } else if (WIFSIGNALED(status)) {
        obj_set(out, "exit", json_num(128.0 + WTERMSIG(status)));
        obj_set(out, "signal", json_num(static_cast<double>(WTERMSIG(status))));
    }
    obj_set(out, "stdout", json_str(head));
    if (truncated) {
        obj_set(out, "truncated", json_bool(true));
        obj_set(out, "output_bytes", json_num(static_cast<double>(total)));
        obj_set(out, "bytes_dropped", json_num(static_cast<double>(dropped)));
    }
    if (background) obj_set(out, "background_still_running", json_bool(true));

    // A hint only where there is something to act on. A failure names the next move; a cut output
    // names what was lost and that narrowing beats rerunning; a query that printed nothing says the
    // silence means "no match", which is the case that reads as success and is not.
    if (error.empty()) {
        const int code = WIFEXITED(status) ? WEXITSTATUS(status)
                       : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 0;
        if (code != 0) {
            std::string hint;
            if (const auto h = annotate_failure(cmd, code, head)) hint = *h;
            if (truncated) {
                if (!hint.empty()) hint += " ";
                hint += "The output was cut at " + std::to_string(keep) + " bytes (" +
                        std::to_string(dropped) + " dropped), so the reason may sit past the cut — narrow "
                        "the command instead of rerunning it whole.";
            }
            if (!hint.empty()) obj_set(out, "hint", json_str(hint));
        } else if (truncated) {
            obj_set(out, "hint", json_str("Output cut at " + std::to_string(keep) + " bytes: " +
                        std::to_string(dropped) + " of " + std::to_string(total) + " dropped. Narrow the "
                        "command (a path, `grep -n`, `head`) rather than rerunning it — the dropped tail "
                        "cannot be recovered from this result."));
        } else if (head.empty() && is_query_command(cmd)) {
            obj_set(out, "stdout_empty", json_bool(true));
            obj_set(out, "hint", json_str("Exit 0 with no output: the command ran and printed nothing. For a "
                        "query verb that means nothing matched — check the path and the pattern before "
                        "treating this as success."));
        }
    }
    return json_dump(out);
}

void shell_kill_all() {
    for (auto& s : g_groups) {
        const pid_t g = s.exchange(0);
        if (g > 0) ::killpg(g, SIGKILL);
    }
}

}  // namespace tools
}  // namespace dd
