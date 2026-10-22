// src/dd_tui.cpp — the Daedalus C++ harness in a terminal: dd::ui panels over
// the agent loop.
//
//   * tty: full-screen, fixed layout (transcript cards | session/engine/tools
//     sidebar, prompt box, status row) on the alternate screen. agent.run()
//     runs on a worker; the main thread reads keys and presents frames.
//     Worker and tool threads only post events to the scene (lock-free).
//   * `-- msg` or piped stdin: the same cards printed inline, in order.
//   * MCP tools are discovered live (config mcp_servers) and dispatched over
//     Streamable-HTTP; the server's instructions join the system prompt.
//   * Esc / Ctrl-C interrupt a turn, including an LLM call in flight.
//
//   build/dd_tui                                 # config.yaml beside the binary, interactive
//   build/dd_tui --init [--from FILE]            # write config.yaml + folders beside the binary
//   build/dd_tui --home DIR                      # another home: config.yaml, skills/, rules/, state/, logs/
//   build/dd_tui path/to/config.yaml -- "msg"    # explicit config (its folder is the home), single-shot msg
//   env: DD_HOME, DD_BASE_URL, DD_MODEL, DD_API_KEY, DD_MAX_TURNS, DD_TUI_COLS, DD_TUI_ROWS,
//        DD_TUI_MOUSE=0 (keep the terminal's own mouse), NO_COLOR
//   commands: /help /tools /turns N /effort L /reasoning L|show|full|hide /clear /interrupt /quit

#include "dd/agent/agent.hpp"
#include "dd/agent/http_llm.hpp"
#include "dd/context/deck.hpp"
#include "dd/context/endpoint.hpp"
#include "dd/context/engine.hpp"
#include "dd/context/maze.hpp"
#include "dd/context/recall.hpp"
#include "dd/core/config.hpp"
#include "dd/core/home.hpp"
#include "dd/core/json.hpp"
#include "dd/core/message.hpp"
#include "dd/know/prompt.hpp"
#include "dd/know/resolver.hpp"
#include "dd/know/router.hpp"
#include "dd/know/tripwire.hpp"
#include "dd/learn/service.hpp"
#include "dd/know/wiring.hpp"
#include "dd/tools/mcp.hpp"
#include "dd/tools/registry.hpp"
#include "dd/tools/tool.hpp"
#include "dd/tools/tool_search.hpp"
#include "dd/ui.hpp"
#include "dd/ui_simd.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <curl/curl.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace ddcore = ::dd::core;
using namespace dd::core;
using dd::tools::ToolEntry;
using dd::tools::MCPClient;

using JsonSP = ddcore::JsonSP;

// ---- tiny builders: Json value -> JsonSP (for init-lists) ----
static JsonSP jsp(Json v) { return std::make_shared<Json>(std::move(v)); }

// The effort ladder /effort and /reasoning offer; "off" goes out as "none",
// which llama-server and Strata both read as thinking disabled.
static const char* const kEfforts[] = {"off", "low", "medium", "high", "xhigh", "max", "ultra"};
constexpr int kEffortCount = 7;

static int effort_index(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (s == "none" || s == "false" || s == "disabled") s = "off";
    for (int i = 0; i < kEffortCount; ++i)
        if (s == kEfforts[i]) return i;
    return -1;
}

static // A soaked turn reads as when and what was asked, not as its label and header.
dd::ui::MemoryRow memory_row(char kind, const dd::context::Hit& h) {
    dd::ui::MemoryRow r{kind, h.id, h.label, h.content, h.score};
    const bool turn = h.label.rfind("auto:turn:", 0) == 0, reasoning = h.label.rfind("auto:reasoning:", 0) == 0;
    if (!turn && !reasoning) return r;
    std::tm t{};   // soaked as "@ 2026-10-20T17:29:59Z": shown in local time
    if (const size_t at = h.content.find(" @ ");
        at != std::string::npos && std::sscanf(h.content.c_str() + at + 3, "%4d-%2d-%2dT%2d:%2d", &t.tm_year, &t.tm_mon,
                                                &t.tm_mday, &t.tm_hour, &t.tm_min) == 5) {
        t.tm_year -= 1900;
        t.tm_mon -= 1;
        const std::time_t utc = timegm(&t);
        std::tm loc{};
        localtime_r(&utc, &loc);
        char when[16];
        std::strftime(when, sizeof when, "%m-%d %H:%M", &loc);
        r.label = std::string(turn ? "turn " : "thought ") + when;
    }
    const size_t u = h.content.find("=== USER === "), a = h.content.find(" === ASSISTANT === ");
    if (u != std::string::npos) {
        const size_t q0 = u + 13;
        r.text = "“" + h.content.substr(q0, (a == std::string::npos ? h.content.size() : a) - q0) + "”";
        if (a != std::string::npos) r.text += " → " + h.content.substr(a + 19);
    } else if (const size_t z = h.content.find("Z "); z != std::string::npos && z < 80) {
        r.text = h.content.substr(z + 2);
        if (r.text.rfind("=== REASONING === ", 0) == 0) r.text.erase(0, 18);
    }
    return r;
}

std::string effort_label(int i) { return i < 0 ? "default" : kEfforts[i]; }

static std::string jerr(const std::string& m) {
    return json_dump(json_obj({{"error", jsp(json_str(m))}}));
}

// a mazemaker tool that changes the maze; unknown ones count as writes
static bool maze_write(const std::string& t) {
    static const std::set<std::string> reads = {
        "mazemaker_recall", "mazemaker_recall_advanced", "mazemaker_recall_multi", "mazemaker_get",
        "mazemaker_think", "mazemaker_stats", "mazemaker_graph", "mazemaker_browse", "mazemaker_health",
        "mazemaker_list_by_label_prefix", "mazemaker_count_by_label_prefix", "mazemaker_afe_facts",
        "mazemaker_classify_intent", "mazemaker_quota", "mazemaker_diagnose", "mazemaker_dream_stats",
        "mazemaker_supersedes_log", "mazemaker_synth_lineage"};
    return t.rfind("mazemaker_", 0) == 0 && !reads.count(t);
}

static double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

static bool result_failed(const std::string& r) {
    try {
        Json j = json_parse(r);
        if (j.type != Json::Obj) return false;
        if (json_get(j, "error")) return true;
        if (const Json* e = json_get(j, "isError"); e && e->type == Json::Bool && e->b) return true;
        if (const Json* x = json_get(j, "exit"); x && x->type == Json::Num && x->num != 0) return true;
    } catch (...) {
    }
    return false;
}

// ---- filesystem tool handlers (real I/O) ----
// Numbered lines "     N|text" from offset (1-based), limit lines at a time:
// a result cut for the window says where to continue.
static dd::know::Resolver* g_resolver = nullptr;   // skill://name, mem://id: one verb reads everything the catalogues hold

static std::string h_read_file(const Json& args) {
    const Json* p = json_get(args, "path");
    if (!p || p->type != Json::Str) return jerr("path required");
    if (g_resolver)
        if (auto internal = g_resolver->read(p->str)) return *internal;
    std::ifstream f(p->str, std::ios::binary);
    if (!f) return jerr("cannot open " + p->str);
    long offset = 1, limit = 2000;
    if (const Json* o = json_get(args, "offset"); o && o->type == Json::Num) offset = std::max(1L, static_cast<long>(o->num));
    if (const Json* l = json_get(args, "limit"); l && l->type == Json::Num) limit = std::max(1L, static_cast<long>(l->num));
    std::string out, line;
    long n = 0, shown = 0;
    char num[24];
    while (std::getline(f, line)) {
        ++n;
        if (n < offset || shown >= limit) continue;
        std::snprintf(num, sizeof num, "%6ld|", n);
        out += num + line + "\n";
        ++shown;
    }
    return json_dump(json_obj({{"path", jsp(json_str(p->str))},
                               {"content", jsp(json_str(out))},
                               {"total_lines", jsp(json_num(static_cast<double>(n)))},
                               {"offset", jsp(json_num(static_cast<double>(offset)))},
                               {"lines", jsp(json_num(static_cast<double>(shown)))}}));
}
static std::string h_write_file(const Json& args) {
    const Json* p = json_get(args, "path");
    const Json* c = json_get(args, "content");
    if (!p || p->type != Json::Str || !c || c->type != Json::Str) return jerr("path+content required");
    std::ofstream o(p->str, std::ios::binary);
    if (!o) return jerr("cannot open " + p->str);
    o << c->str;
    return json_dump(json_obj({{"path", jsp(json_str(p->str))},
                               {"bytes", jsp(json_num((double)c->str.size()))}}));
}
static std::string h_list_dir(const Json& args) {
    const Json* p = json_get(args, "path");
    std::string dir = (p && p->type == Json::Str) ? p->str : std::string(".");
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return jerr("not a dir: " + dir);
    Json arr = json_arr();
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
        arr_push(arr, json_str(e.path().string()));
    return json_dump(json_obj({{"path", jsp(json_str(dir))}, {"entries", jsp(std::move(arr))}}));
}
static Json fs_schema(const std::vector<std::string>& props, const std::string& desc) {
    Json props_map = json_obj();
    for (const auto& k : props) {
        Json one = json_obj({{"type", jsp(json_str("string"))},
                             {"description", jsp(json_str(desc))}});
        obj_set(props_map, k, one);
    }
    obj_set(props_map, "intent", json_obj({{"type", jsp(json_str("string"))},
                                           {"description", jsp(json_str("what you are doing, in a few words (optional)"))}}));
    Json req = json_arr();
    for (const auto& k : props) arr_push(req, json_str(k));
    Json parameters = json_obj({{"type", jsp(json_str("object"))},
                                {"properties", jsp(std::move(props_map))},
                                {"required", jsp(std::move(req))}});
    return json_obj({{"description", jsp(json_str(desc))},
                     {"parameters", jsp(std::move(parameters))}});
}

// ---- guarded shell tool: everything allowed except a denylist of dangerous commands ----
static bool has_recursive_flag(const std::vector<std::string>& t) {
    for (const auto& a : t) {
        if (a == "--recursive") return true;
        if (a.size() > 1 && a[0] == '-' && a[1] != '-' && a.find_first_of("rR") != std::string::npos) return true;
    }
    return false;
}

static bool is_broad_target(const std::string& a) {
    static const char* const kBroad[] = {"/", "/*", "~", "~/", "~/*", "$HOME", "$HOME/", "*", ".", "..", "./*",
        "/home", "/etc", "/usr", "/var", "/boot", "/bin", "/sbin", "/lib", "/lib64", "/opt", "/root", "/dev", "/sys", "/proc"};
    for (const char* b : kBroad) if (a == b) return true;
    return false;
}

// Returns the reason a command is refused, or "" when it may run.
static std::string danger_reason(const std::string& cmd, const ddcore::Config& cfg) {
    for (const char* bad : {":(){", "> /dev/sd", ">/dev/sd", "> /dev/nvme", ">/dev/nvme", "of=/dev/", "--no-preserve-root"})
        if (cmd.find(bad) != std::string::npos) return std::string("forbidden pattern '") + bad + "'";
    if ((cmd.find("curl") != std::string::npos || cmd.find("wget") != std::string::npos) &&
        (cmd.find("| sh") != std::string::npos || cmd.find("|sh") != std::string::npos ||
         cmd.find("| bash") != std::string::npos || cmd.find("|bash") != std::string::npos))
        return "piping a download into a shell";

    static const std::set<std::string> kDeny = {
        "sudo", "su", "doas", "pkexec", "mkswap", "fdisk", "sfdisk", "parted", "wipefs", "shred", "dd",
        "shutdown", "reboot", "poweroff", "halt", "init", "telinit", "killall5", "passwd", "chpasswd",
        "visudo", "useradd", "userdel", "usermod", "docker"};
    static const std::set<std::string> kWrap = {"env", "command", "nohup", "exec", "time", "nice", "xargs",
        "timeout", "setsid", "stdbuf", "builtin"};
    std::set<std::string> extra;
    if (const Json* d = cfg.get_list("command_denylist"); d && d->type == Json::Arr)
        for (const auto& sp : d->arr) if (sp && sp->type == Json::Str) extra.insert(sp->str);

    std::string flat = cmd;
    for (char& ch : flat)
        if (ch == ';' || ch == '&' || ch == '|' || ch == '(' || ch == ')' || ch == '`' || ch == '\n') ch = '\n';
    std::istringstream segs(flat);
    std::string seg;
    while (std::getline(segs, seg)) {
        std::vector<std::string> t;
        std::istringstream ts(seg);
        for (std::string w; ts >> w;) t.push_back(w);
        std::size_t i = 0;
        while (i < t.size()) {  // skip VAR=val and wrappers (plus their flags/numbers)
            const std::string& w = t[i];
            auto eq = w.find('=');
            if (eq != std::string::npos && eq > 0 && w.find('/') > eq) { ++i; continue; }
            if (kWrap.count(w)) {
                ++i;
                while (i < t.size() && (t[i][0] == '-' || std::isdigit((unsigned char)t[i][0]))) ++i;
                continue;
            }
            break;
        }
        if (i >= t.size()) continue;
        std::string word = t[i];
        if (auto s = word.rfind('/'); s != std::string::npos) word = word.substr(s + 1);
        std::vector<std::string> args(t.begin() + i + 1, t.end());

        if (kDeny.count(word) || extra.count(word) || word.rfind("mkfs", 0) == 0)
            return "command '" + word + "' is blocked";
        auto any_broad = [&] { for (const auto& a : args) if (is_broad_target(a)) return true; return false; };
        if (word == "rm" && has_recursive_flag(args) && any_broad()) return "recursive rm on a broad path";
        if ((word == "chmod" || word == "chown" || word == "chgrp") && has_recursive_flag(args) && any_broad())
            return "recursive " + word + " on a broad path";
        if (word == "kill" || word == "pkill")
            for (const auto& a : args) if (a == "-1" || a == "1") return "signalling pid 1 / all processes";
        if (word == "systemctl")
            for (const auto& a : args)
                if (a == "poweroff" || a == "reboot" || a == "halt" || a == "suspend" || a == "hibernate" || a == "kexec")
                    return "systemctl " + a;
        if (word == "podman" && args.size() >= 2 && args[0] == "system" && args[1] == "reset") return "podman system reset";
        if (word == "git") {
            bool push = false;
            for (const auto& a : args) {
                if (a == "push") push = true;
                if (a == "--no-verify") return "git --no-verify";
                if (push && (a == "--force" || a == "-f" || a == "--force-with-lease")) return "git force-push";
            }
        }
    }
    return "";
}

static std::string run_terminal(const Json& args, const ddcore::Config& cfg) {
    const Json* c = json_get(args, "cmd");
    if (!c || c->type != Json::Str || c->str.empty()) return jerr("cmd required");
    std::string cmd = c->str;

    if (std::string why = danger_reason(cmd, cfg); !why.empty()) return jerr("refused: " + why);

    // a group: stdin and stderr apply to every command in it, and the child
    // never reads the tty the TUI holds in raw mode
    std::string full = "{ " + cmd + "\n} </dev/null 2>&1";
    FILE* fp = popen(full.c_str(), "r");
    if (!fp) return jerr("popen failed");
    std::string out;
    char buf[4096];
    while (size_t n = fread(buf, 1, sizeof buf, fp)) out.append(buf, n);
    int rc = pclose(fp);
    if (out.size() > 8000) out = out.substr(0, 8000) + "...(truncated)";
    return json_dump(json_obj({{"cmd", jsp(json_str(cmd))},
                               {"exit", jsp(json_num((double)rc))},
                               {"stdout", jsp(json_str(out))}}));
}

// ---- terminal: raw mode on the alternate screen, restored on every exit ----
namespace term {

termios g_orig{};
std::atomic<bool> g_active{false};

// alt screen, no autowrap, bracketed paste, bar cursor
constexpr char kEnter[] = "\x1b[?1049h\x1b[?7l\x1b[?2004h\x1b[6 q\x1b[?25l";
constexpr char kMouseOn[] = "\x1b[?1000h\x1b[?1006h";   // wheel, SGR coordinates
constexpr char kLeave[] = "\x1b[?1000l\x1b[?1006l\x1b[?2004l\x1b[?7h\x1b[0 q\x1b[0m\x1b[?25h\x1b[?1049l";

void put(const char* s, size_t n) {
    while (n) {
        const ssize_t w = ::write(STDOUT_FILENO, s, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return;
        s += w;
        n -= static_cast<size_t>(w);
    }
}
void put(const std::string& s) { put(s.data(), s.size()); }

void leave() {   // async-signal-safe
    if (!g_active.exchange(false)) return;
    put(kLeave, sizeof kLeave - 1);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig);
}

void on_fatal(int sig) {
    leave();
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

bool enter(bool mouse) {
    if (tcgetattr(STDIN_FILENO, &g_orig) != 0) return false;
    termios raw = g_orig;
    raw.c_iflag &= ~static_cast<tcflag_t>(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return false;
    g_active = true;
    for (int s : {SIGTERM, SIGHUP, SIGQUIT, SIGSEGV, SIGABRT, SIGBUS, SIGFPE}) std::signal(s, on_fatal);
    put(kEnter, sizeof kEnter - 1);
    if (mouse) put(kMouseOn, sizeof kMouseOn - 1);
    return true;
}

// stderr would scribble over the frame: send it to a log while we draw.
int hide_stderr(const std::string& dir) {
    std::error_code ec;
    int fd = -1;
    if (std::filesystem::is_directory(dir, ec))
        fd = ::open((dir + "/dd_tui.log").c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) fd = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    const int saved = ::dup(STDERR_FILENO);
    ::dup2(fd, STDERR_FILENO);
    ::close(fd);
    return saved;
}

void show_stderr(int saved) {
    if (saved < 0) return;
    ::dup2(saved, STDERR_FILENO);
    ::close(saved);
}

void size(int& cols, int& rows) {
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        cols = ws.ws_col;
        rows = ws.ws_row;
    }
    if (const char* v = std::getenv("DD_TUI_COLS")) cols = std::max(1, std::atoi(v));
    if (const char* v = std::getenv("DD_TUI_ROWS")) rows = std::max(1, std::atoi(v));
}

}  // namespace term

// ---- keys: raw bytes -> key events (CSI/SS3, SGR mouse, bracketed paste, UTF-8) ----
enum class K { Char, Paste, Enter, Backspace, Delete, Left, Right, WordLeft, WordRight, Up, Down, Home, End,
               PgUp, PgDn, WheelUp, WheelDown, Tab, Esc, CtrlC, CtrlD, CtrlK, CtrlL, CtrlU, CtrlW };
struct Key {
    K k;
    std::u32string text;
};

class KeyReader {
public:
    // Complete keys from what has arrived; a split sequence waits for the rest.
    std::vector<Key> feed(const char* p, size_t n) {
        buf_.append(p, n);
        std::vector<Key> out;
        auto key = [&](K k) { out.push_back(Key{k, {}}); };
        size_t i = 0;
        while (i < buf_.size()) {
            if (in_paste_) {
                const size_t e = buf_.find("\x1b[201~", i);
                if (e == std::string::npos) {
                    const size_t keep = std::min<size_t>(5, buf_.size() - i);   // maybe half a terminator
                    paste_.append(buf_, i, buf_.size() - i - keep);
                    i = buf_.size() - keep;
                    break;
                }
                paste_.append(buf_, i, e - i);
                std::string t;
                for (size_t k = 0; k < paste_.size(); ++k) {
                    if (paste_[k] == '\r') {
                        t.push_back('\n');
                        if (k + 1 < paste_.size() && paste_[k + 1] == '\n') ++k;
                    } else {
                        t.push_back(paste_[k]);
                    }
                }
                paste_.clear();
                out.push_back(Key{K::Paste, dd::ui::from_utf8(t)});
                in_paste_ = false;
                i = e + 6;
                continue;
            }
            const unsigned char c = static_cast<unsigned char>(buf_[i]);
            if (c == 0x1b) {
                if (i + 1 >= buf_.size()) { key(K::Esc); ++i; continue; }
                const char n1 = buf_[i + 1];
                if (n1 == '[') {
                    size_t j = i + 2;
                    while (j < buf_.size() && (static_cast<unsigned char>(buf_[j]) < 0x40 || static_cast<unsigned char>(buf_[j]) > 0x7E)) ++j;
                    if (j >= buf_.size()) break;
                    const std::string par = buf_.substr(i + 2, j - i - 2);
                    const char fin = buf_[j];
                    i = j + 1;
                    if (!par.empty() && par[0] == '<') {
                        const int b = std::atoi(par.c_str() + 1);
                        if (fin == 'M' && b == 64) key(K::WheelUp);
                        if (fin == 'M' && b == 65) key(K::WheelDown);
                        continue;
                    }
                    const bool ctrl = par.find(";5") != std::string::npos;
                    switch (fin) {
                    case 'A': key(K::Up); break;
                    case 'B': key(K::Down); break;
                    case 'C': key(ctrl ? K::WordRight : K::Right); break;
                    case 'D': key(ctrl ? K::WordLeft : K::Left); break;
                    case 'H': key(K::Home); break;
                    case 'F': key(K::End); break;
                    case '~': {
                        const int code = std::atoi(par.c_str());
                        if (code == 1 || code == 7) key(K::Home);
                        else if (code == 4 || code == 8) key(K::End);
                        else if (code == 3) key(K::Delete);
                        else if (code == 5) key(K::PgUp);
                        else if (code == 6) key(K::PgDn);
                        else if (code == 200) in_paste_ = true;
                        break;
                    }
                    default: break;
                    }
                    continue;
                }
                if (n1 == 'O') {
                    if (i + 2 >= buf_.size()) break;
                    switch (buf_[i + 2]) {
                    case 'A': key(K::Up); break;
                    case 'B': key(K::Down); break;
                    case 'C': key(K::Right); break;
                    case 'D': key(K::Left); break;
                    case 'H': key(K::Home); break;
                    case 'F': key(K::End); break;
                    default: break;
                    }
                    i += 3;
                    continue;
                }
                if (n1 == 0x7f || n1 == 0x08) key(K::CtrlW);   // alt-backspace
                else if (n1 == 'b') key(K::WordLeft);
                else if (n1 == 'f') key(K::WordRight);
                i += 2;
                continue;
            }
            if (c == '\r' || c == '\n') { key(K::Enter); ++i; continue; }
            if (c == 0x7f || c == 0x08) { key(K::Backspace); ++i; continue; }
            if (c < 0x20) {
                switch (c) {
                case '\t': key(K::Tab); break;
                case 0x01: key(K::Home); break;
                case 0x02: key(K::Left); break;
                case 0x03: key(K::CtrlC); break;
                case 0x04: key(K::CtrlD); break;
                case 0x05: key(K::End); break;
                case 0x06: key(K::Right); break;
                case 0x0b: key(K::CtrlK); break;
                case 0x0c: key(K::CtrlL); break;
                case 0x0e: key(K::Down); break;
                case 0x10: key(K::Up); break;
                case 0x15: key(K::CtrlU); break;
                case 0x17: key(K::CtrlW); break;
                default: break;
                }
                ++i;
                continue;
            }
            const size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
            if (i + len > buf_.size()) break;
            out.push_back(Key{K::Char, dd::ui::from_utf8(buf_.substr(i, len))});
            i += len;
        }
        buf_.erase(0, i);
        return out;
    }

private:
    std::string buf_, paste_;
    bool in_paste_ = false;
};

// ---- the prompt line: UTF-8 aware editing + history ----
struct Editor {
    std::u32string buf;
    size_t cur = 0;
    std::vector<std::u32string> hist;
    size_t hpos = 0;
    std::u32string draft;

    void insert(const std::u32string& s) { buf.insert(cur, s); cur += s.size(); }
    void backspace() { if (cur) buf.erase(--cur, 1); }
    void del() { if (cur < buf.size()) buf.erase(cur, 1); }
    void word_left() {
        while (cur && buf[cur - 1] == U' ') --cur;
        while (cur && buf[cur - 1] != U' ') --cur;
    }
    void word_right() {
        while (cur < buf.size() && buf[cur] == U' ') ++cur;
        while (cur < buf.size() && buf[cur] != U' ') ++cur;
    }
    void kill_word() {
        const size_t e = cur;
        word_left();
        buf.erase(cur, e - cur);
    }
    void clear() {
        buf.clear();
        cur = 0;
        hpos = hist.size();
    }
    void recall(int dir) {
        if (hist.empty()) return;
        if (hpos == hist.size()) draft = buf;
        if (dir < 0 && hpos > 0) --hpos;
        else if (dir > 0 && hpos < hist.size()) ++hpos;
        else return;
        buf = hpos == hist.size() ? draft : hist[hpos];
        cur = buf.size();
    }
    std::u32string take() {
        std::u32string s = buf;
        if (!s.empty() && (hist.empty() || hist.back() != s)) hist.push_back(s);
        clear();
        return s;
    }
};

static const char* const kCommands[] = {"/help", "/tools", "/turns", "/effort", "/reasoning", "/context", "/rebase",
                                        "/resume", "/clear", "/new", "/interrupt", "/quit"};

static std::string trim_ws(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

// "/foo" with no second slash is a command; "/home/x ..." is a prompt.
static bool is_command(const std::string& p) {
    if (p.empty() || p[0] != '/') return false;
    const std::string first = p.substr(0, p.find(' '));
    return first.find('/', 1) == std::string::npos;
}

static std::vector<std::string> command_matches(const std::string& typed) {
    std::vector<std::string> m;
    for (const char* c : kCommands)
        if (std::string(c).rfind(typed, 0) == 0) m.emplace_back(c);
    return m;
}

// "/effort x" / "/reasoning x": the arguments that fit the prefix x.
static std::vector<std::string> arg_matches(const std::string& typed, const std::vector<std::string>& efforts) {
    std::vector<std::string> m;
    const size_t sp = typed.find(' ');
    if (sp == std::string::npos || typed.find(' ', sp + 1) != std::string::npos) return m;
    const std::string cmd = typed.substr(0, sp), pre = typed.substr(sp + 1);
    if (cmd != "/effort" && cmd != "/reasoning") return m;
    std::vector<std::string> all = efforts;
    if (cmd == "/reasoning") all.insert(all.end(), {"show", "full", "hide"});
    for (const auto& a : all)
        if (a.rfind(pre, 0) == 0) m.push_back(a);
    return m;
}

int main(int argc, char** argv) {
    curl_global_init(CURL_GLOBAL_DEFAULT);   // before any thread touches curl
    std::string home_flag, config_flag, init_from;
    bool init = false, init_rules = false;
    std::vector<std::string> msg_words;
    bool after_dd = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (after_dd) msg_words.push_back(a);
        else if (a == "--") after_dd = true;
        else if (a == "--home" && i + 1 < argc) home_flag = argv[++i];
        else if (a == "--config" && i + 1 < argc) config_flag = argv[++i];
        else if (a == "--init") init = true;
        else if (a == "--rules") init_rules = true;
        else if (a == "--from" && i + 1 < argc) init_from = argv[++i];
        else config_flag = a;   // a bare path is the config, as it always was
    }

    // Beside the binary unless told otherwise; never the Python Daedalus's ~/.daedalus.
    const ddcore::Home home = ddcore::resolve_home(home_flag, config_flag);
    ddcore::set_daedalus_home_override(home.root.string());   // the ported modules agree on it
    const std::string config_path = config_flag.empty() ? home.config().string() : config_flag;
    {
        std::string why;
        if (!ddcore::ensure_home(home, &why)) {
            std::cerr << "dd_tui: " << why << "\n  pass --home DIR for a folder you can write\n";
            return 2;
        }
        if (init) {
            const bool wrote = ddcore::init_config(home, init_from, &why);
            std::cout << (wrote ? "wrote " : "kept ") << config_path << (wrote || why.empty() ? "" : "  (" + why + ")") << "\n";
            if (init_rules) {   // the operator's first tripwires: opt-in, they are one person's habits
                std::error_code rec;
                std::filesystem::create_directories(home.rules(), rec);
                int made = 0;
                for (const auto& [name, text] : dd::know::Tripwires::starter_rules()) {
                    const std::filesystem::path f = home.rules() / name;
                    if (std::filesystem::exists(f, rec)) continue;
                    std::ofstream(f, std::ios::binary) << text;
                    ++made;
                }
                std::cout << "rules: " << made << " written to " << home.rules().string() << "\n";
            }
            return wrote || std::filesystem::exists(config_path) ? 0 : 2;
        }
        std::error_code ec;
        if (!std::filesystem::exists(config_path, ec) && !std::getenv("DD_BASE_URL")) {
            std::cerr << "dd_tui: no config at " << config_path << "\n  create one: dd_tui --init [--from FILE]   "
                      << "(or --config FILE, --home DIR)\n";
            return 2;
        }
    }

    ddcore::Config cfg{ddcore::Json{}};
    try { cfg = ddcore::load_config(config_path); }
    catch (const std::exception& ex) { std::cerr << "config: " << ex.what() << "\n"; }

    auto env_or = [](const char* k, const std::string& def) -> std::string {
        const char* v = std::getenv(k);
        return (v && *v) ? std::string(v) : def;
    };
    std::string base_url = env_or("DD_BASE_URL", cfg.get_string("model.base_url"));
    std::string model = env_or("DD_MODEL", cfg.get_string("model.name"));
    std::string api_key = env_or("DD_API_KEY", cfg.get_string("model.api_key"));
    long long mt = 0;
    if (const char* v = std::getenv("DD_MAX_TURNS")) mt = std::stoll(v);
    else {
        long long it = cfg.get_int("agent.max_iterations");
        if (it <= 0) it = cfg.get_int("agent.max_turns");
        mt = it;
    }
    int max_iterations = (mt > 0) ? (int)mt : 50;

    if (base_url.empty()) { std::cerr << "no model.base_url and DD_BASE_URL unset\n"; return 2; }

    const bool to_tty = isatty(STDOUT_FILENO);
    const bool interactive = msg_words.empty() && isatty(STDIN_FILENO) && to_tty;
    const char* ct = std::getenv("COLORTERM");
    const bool truecolor = ct && (std::strcmp(ct, "truecolor") == 0 || std::strcmp(ct, "24bit") == 0);
    const bool color = to_tty && !std::getenv("NO_COLOR");

    const std::string skin = cfg.get_string("display.skin");
    dd::ui::Scene scene(dd::ui::Theme::load((home.skins() / ((skin.empty() ? "neural" : skin) + ".yaml")).string()));

    // ---- the registry; every handler reports its run to the scene ----
    dd::tools::ToolRegistry reg;
    auto add_tool = [&](ToolEntry e) {
        auto inner = std::move(e.handler);
        e.handler = [inner, name = e.name, &scene](const Json& a) {
            const uint64_t tok = scene.tool_begin(name);
            const auto t0 = std::chrono::steady_clock::now();
            std::string r;
            try {
                r = inner(a);
            } catch (...) {
                scene.tool_end(tok, ms_since(t0), false);
                throw;
            }
            scene.tool_end(tok, ms_since(t0), !result_failed(r));
            return r;
        };
        reg.register_tool(e);
    };
    {
        ToolEntry e;
        e.name = "read_file";
        const std::string desc = "Read a text file as numbered lines (\"     N|text\"). Long files: page with offset "
                                 "(first line, 1-based) and limit (lines, default 2000). Also reads what the harness points to: "
                                 "skill://name (a page at a time: #2, #all; its files: skill://name/references/x.md) and mem://id.";
        Json props = json_obj({{"path", jsp(json_obj({{"type", jsp(json_str("string"))}, {"description", jsp(json_str("file to read, or skill://name, mem://id"))}}))},
                               {"intent", jsp(json_obj({{"type", jsp(json_str("string"))}, {"description", jsp(json_str("what you are doing, in a few words (optional)"))}}))},
                               {"offset", jsp(json_obj({{"type", jsp(json_str("integer"))}, {"description", jsp(json_str("first line, 1-based"))}}))},
                               {"limit", jsp(json_obj({{"type", jsp(json_str("integer"))}, {"description", jsp(json_str("how many lines"))}}))}});
        e.schema = json_obj({{"description", jsp(json_str(desc))},
                             {"parameters", jsp(json_obj({{"type", jsp(json_str("object"))},
                                                          {"properties", jsp(std::move(props))},
                                                          {"required", jsp(json_arr({jsp(json_str("path"))}))}}))}});
        e.handler = h_read_file;
        add_tool(e);
    }
    { ToolEntry e; e.name = "write_file"; e.schema = fs_schema({"path", "content"}, "Write text content to a file."); e.handler = h_write_file; add_tool(e); }
    { ToolEntry e; e.name = "list_dir"; e.schema = fs_schema({"path"}, "List entries of a directory."); e.handler = h_list_dir; add_tool(e); }
    {
        ToolEntry e;
        e.name = "run_terminal";
        {
            Json rt = json_arr();
            arr_push(rt, json_str("cmd"));
            Json cmdp = json_obj({{"type", jsp(json_str("string"))},
                                  {"description", jsp(json_str("command to run"))}});
            Json props = json_obj({{"cmd", jsp(std::move(cmdp))},
                                   {"intent", jsp(json_obj({{"type", jsp(json_str("string"))},
                                                            {"description", jsp(json_str("what you are doing, in a few words (optional)"))}}))}});
            Json params = json_obj({{"type", jsp(json_str("object"))},
                                    {"properties", jsp(std::move(props))},
                                    {"required", jsp(std::move(rt))}});
            e.schema = json_obj({{"description", jsp(json_str("Run a shell command. Dangerous commands (sudo, su, rm -rf /, mkfs, dd, shutdown, force-push, ...) are refused. Returns exit code + stdout."))},
                                 {"parameters", jsp(std::move(params))}});
        }
        e.handler = [&cfg](const Json& a) { return run_terminal(a, cfg); };
        add_tool(e);
    }
    std::vector<std::string> sources = {"local  4"};
    std::vector<std::string> boot_errors;

    // ---- discover + register the live MCP servers from config ----
    std::string system_prompt =
        "Tool use: for LIVE state (VRAM/GPU, RAM, disk, processes, files, the running model and its context "
        "size) call run_terminal or read_file FIRST, e.g. `nvidia-smi --query-gpu=memory.used,memory.total "
        "--format=csv`, `free -h`, `curl -s http://127.0.0.1:8080/props`.";
    if (std::string cp = cfg.get_string("agent.system_prompt"); !cp.empty()) system_prompt += "\n\n" + cp;
    std::vector<std::unique_ptr<MCPClient>> mcps;  // stable addresses
    struct ServerNote {
        std::string url, text, mode;   // mode: the server's `instructions:` setting
    };
    std::vector<ServerNote> server_notes;
    std::string maze_url;                           // the server that answers mazemaker_recall
    if (const Json* srvs = cfg.get_map("mcp_servers"); srvs && srvs->type == Json::Obj) {
        for (const auto& [srvname, sroot] : srvs->obj) {
            if (!sroot || sroot->type != Json::Obj) continue;
            const Json& sv = *sroot;
            if (const Json* e = json_get(sv, "enabled"); e && e->type == Json::Bool && !e->b) continue;
            if (const Json* tr = json_get(sv, "transport"); tr && tr->type == Json::Str && tr->str != "http")
                continue;  // this client speaks Streamable-HTTP only
            const Json* u = json_get(sv, "url");
            if (!u || u->type != Json::Str || u->str.empty()) continue;

            auto mcp = std::make_unique<MCPClient>();
            mcp->url = u->str;
            try {
                // what to do with the server's own protocol text depends on whether it is the maze: decided after the loop
                if (const std::string instr = mcp->instructions(); !instr.empty()) {
                    const Json* ic = json_get(sv, "instructions");
                    server_notes.push_back({u->str, instr, ic && ic->type == Json::Str ? ic->str : std::string()});
                }
                Json tl = mcp->list_tools();
                std::size_t registered = 0;
                if (tl.type == Json::Arr) {
                    const Json* tmap = json_get(sv, "tools");  // name -> {enabled}
                    for (const auto& tsp : tl.arr) {
                        if (!tsp || tsp->type != Json::Obj) continue;
                        const Json* nm = json_get(*tsp, "name");
                        if (!nm || nm->type != Json::Str) continue;
                        const std::string tname = nm->str;
                        if (tname == "mazemaker_recall") maze_url = u->str;
                        if (tmap) {
                            if (const Json* en = json_get(*tmap, tname))
                                if (en->type == Json::Obj)
                                    if (const Json* on = json_get(*en, "enabled"))
                                        if (on->type == Json::Bool && !on->b) continue;
                        }
                        const Json* desc = json_get(*tsp, "description");
                        const Json* ischema = json_get(*tsp, "inputSchema");
                        Json entry_schema = json_obj();
                        if (desc && desc->type == Json::Str) obj_set(entry_schema, "description", json_str(desc->str));
                        if (ischema && ischema->type == Json::Obj)
                            obj_set(entry_schema, "parameters", dd::core::json_deep_copy(*ischema));
                        else
                            obj_set(entry_schema, "parameters", json_obj({{"type", jsp(json_str("object"))}}));

                        ToolEntry e;
                        e.name = tname;
                        e.toolset = srvname;
                        e.schema = std::move(entry_schema);
                        // DD_NO_SOAK: the model's own writes are off too; the schema stays, so the prompt is the real one
                        e.handler = [m = mcp.get(), tname, ro = std::getenv("DD_NO_SOAK") && maze_write(tname)](const Json& a) {
                            return ro ? jerr("DD_NO_SOAK: this run writes nothing to the maze") : m->call(tname, a);
                        };
                        add_tool(e);
                        ++registered;
                    }
                }
                sources.push_back(srvname + "  " + std::to_string(registered));
                mcps.push_back(std::move(mcp));
            } catch (const std::exception& ex) {
                boot_errors.push_back("mcp server '" + srvname + "' unavailable: " + ex.what());
            }
        }
    }

    for (const auto& n : server_notes) {   // verbatim only when asked: the maze's protocol is the harness's contract below
        const auto mode = dd::know::PromptComposer::instructions_mode(n.mode, n.url == maze_url);
        if (const std::string text = dd::know::PromptComposer::server_instructions(mode, n.text); !text.empty())
            system_prompt += (system_prompt.empty() ? "" : "\n\n") + text;
    }

    // ---- the server, and what each request tells it ----
    dd::context::Endpoint ep(base_url);
    ep.probe();
    ep.thinking_budget = cfg.has("agent.thinking_budget") ? static_cast<int>(cfg.get_int("agent.thinking_budget")) : 1024;
    const std::vector<std::string> efforts = ep.efforts();
    auto offered = [&](int i) {
        return i >= 0 && std::find(efforts.begin(), efforts.end(), kEfforts[i]) != efforts.end();
    };
    std::atomic<int> effort{-1};
    if (const std::string want = cfg.get_string("agent.reasoning_effort"); !want.empty()) {
        int i = effort_index(want);
        while (i > 0 && !offered(i)) --i;   // round down to the template's ceiling
        if (offered(i)) effort = i;
        if (i != effort_index(want))
            boot_errors.push_back("agent.reasoning_effort '" + want + "' is not in this model's template; using " +
                                  effort_label(effort.load()));
    }
    if (cfg.get_string("display.show_reasoning") == "false") scene.set_reason_view(dd::ui::ReasonView::Hide);

    // ---- tools in the window: the harness's own + always_load; the rest behind the bridges ----
    const Json* ts_raw = cfg.get_map("tool_search");
    dd::context::ToolDeck deck(reg, dd::tools::ToolSearchConfig::from_raw(ts_raw ? *ts_raw : Json{}));
    deck.registrar = [&](const ToolEntry& e) { add_tool(e); };

    // ---- the maze: what ages out of the window comes back from here ----
    auto maze = std::make_shared<dd::context::Maze>(maze_url);
    if (maze->configured()) maze->probe();

    // ---- what the harness looks up for the model: catalogues, one router, one verb to read them ----
    const dd::know::SkillSetup skill_cfg = dd::know::skill_setup(cfg, home);
    std::shared_ptr<dd::know::SkillCatalogue> skill_cat;
    std::shared_ptr<dd::know::MemoryCatalogue> memory_cat;
    if (skill_cfg.enabled) skill_cat = dd::know::make_skill_catalogue(skill_cfg, maze);
    if (maze->configured()) memory_cat = std::make_shared<dd::know::MemoryCatalogue>(maze, home.cache() / "memory-cards.jsonl", skill_cfg.page_bytes);
    auto tool_cat = std::make_shared<dd::know::ToolCatalogue>(deck);
    dd::know::Router router({skill_cat, memory_cat, tool_cat}, dd::know::Policy::from(cfg), maze);
    dd::know::Resolver resolver(router);
    g_resolver = &resolver;
    {
        ToolEntry e;
        e.name = "find";
        e.schema = dd::know::Resolver::find_schema();
        e.handler = [&resolver](const Json& a) { return resolver.find(a); };
        add_tool(e);
    }
    {
        ToolEntry e;
        e.name = "skill_load";
        e.schema = dd::know::Resolver::skill_load_schema();
        e.handler = [&resolver](const Json& a) { return resolver.skill_load(a); };
        add_tool(e);
    }
    deck.arrange({"read_file", "write_file", "list_dir", "run_terminal", "find", "skill_load"});
    tool_cat->refresh();   // local and instant: searchable at once, and the one catalogue that reads the deck
    if (memory_cat) memory_cat->preload();   // the cache is local too: searchable before any pod call
    // the router learns what was worth offering (priors, terms for the skills that were found by other words); the overlay
    // must be in place before the first refresh, the service itself starts after the refreshers (it is stopped first)
    const bool learn_on = !cfg.has("learn.enabled") || cfg.get_bool("learn.enabled");
    auto learn_overlay = std::make_shared<dd::learn::LearnOverlay>();
    if (learn_on && skill_cat) skill_cat->set_overlay(learn_overlay);
    // each catalogue fed by the maze refreshes on its own thread, again when a source comes back. Declared after the deck
    // and the router, so they are stopped before either goes.
    std::vector<std::unique_ptr<dd::know::Refresher>> refreshers;
    auto start_refresher = [&](const std::shared_ptr<dd::know::Catalogue>& c) {
        refreshers.push_back(std::make_unique<dd::know::Refresher>(std::vector<std::shared_ptr<dd::know::Catalogue>>{c}));
        refreshers.back()->start();
    };
    // the skills go first and get the pod to themselves: the catalogue map goes into the system prompt, which never changes
    // during a session, so give their first look a moment, asleep until it is published (not polling). The memory sync follows.
    if (skill_cat) {
        start_refresher(skill_cat);
        skill_cat->wait_ready(std::chrono::seconds(4));
    }
    if (memory_cat) start_refresher(memory_cat);
    std::unique_ptr<dd::learn::LearnService> learner;
    if (learn_on) {
        dd::learn::ServiceOptions lo;
        lo.dir = home.learn();
        lo.skills_root = home.skills();
        lo.overlay = learn_overlay;
        lo.project_dir = std::filesystem::current_path().string();
        if (const char* h = std::getenv("HOME")) lo.home_dir = h;
        lo.feedback = !cfg.has("learn.feedback") || cfg.get_bool("learn.feedback");
        lo.author = !cfg.has("learn.author") || cfg.get_bool("learn.author");
        if (cfg.has("learn.max_drafts_per_day")) lo.max_drafts_per_day = static_cast<int>(cfg.get_int("learn.max_drafts_per_day"));
        lo.search = [&router](const dd::know::Query& q) {   // the duplicate check asks the catalogue
            std::string text;
            for (const auto& t : q.terms) text += (text.empty() ? "" : " ") + t.text;
            return router.find(text, {dd::know::Kind::Skill}, 5);
        };
        if (!refreshers.empty() && skill_cat) lo.on_skill = [r = refreshers.front().get()] { r->poke(); };
        std::string lerr;
        learner = dd::learn::LearnService::start(std::move(lo), &lerr);
        if (learner) router.set_sink(learner->sink());
    }
    const std::string work = home.state().string();   // spill and the soak spool: beside the binary, not the launch dir
    dd::context::Soaker soaker(maze, work + "/soak-spool.jsonl");

    // ---- the transport ----
    dd::agent::Agent agent;
    dd::agent::HttpLLM llm;
    llm.base_url = base_url;
    llm.model = model;
    llm.api_key = api_key;
    llm.cancel = &agent.interrupt_requested;
    llm.effort = [&effort]() -> std::string {
        const int e = effort.load(std::memory_order_relaxed);
        return e < 0 ? std::string() : e == 0 ? std::string("none") : std::string(kEfforts[e]);
    };
    llm.shape = [&ep, &effort](Json& b) { ep.shape(b, ep.main_slot(), effort.load(std::memory_order_relaxed) != 0); };
    llm.on_begin = [&scene] { scene.llm_begin(); };
    llm.on_reasoning = [&scene](const std::string& d) { scene.llm_reasoning(d); };
    llm.on_content = [&scene](const std::string& d) { scene.llm_content(d); };

    // ---- the context tape ----
    auto num_or = [&](const char* key, double def) {
        return cfg.has(key) ? cfg.get_double(key) : def;
    };
    dd::context::EngineConfig ec;
    ec.tape = !cfg.has("context.payload_tape") || cfg.get_bool("context.payload_tape");
    ec.render.window_turns = cfg.has("memory.soak.window_turns") ? static_cast<int>(cfg.get_int("memory.soak.window_turns")) : -1;
    ec.render.reasoning_window = static_cast<int>(num_or("context.reasoning_window", 3));
    ec.render.tool_group_ttl = static_cast<int>(num_or("context.tool_group_ttl", 8));
    ec.render.context_length = ep.info().n_ctx;
    ec.fast_layer_tokens = static_cast<long>(num_or("compression.fast_layer_tokens", 0));
    ec.low = num_or("compression.watermarks.low", 0.60);
    ec.high = num_or("compression.watermarks.high", 0.80);
    ec.hard = num_or("compression.watermarks.hard", 0.90);
    ec.ceiling = num_or("compression.hot_swap_hard_ratio", 0.85);
    ec.protect_last_n = static_cast<int>(num_or("compression.protect_last_n", 6));
    ec.turn_recall = !cfg.has("memory.turn_recall") || cfg.get_bool("memory.turn_recall");
    ec.prewarm = !cfg.has("compression.prewarm") || cfg.get_bool("compression.prewarm");
    if (const char* v = std::getenv("DD_FAST_LAYER")) ec.fast_layer_tokens = std::atol(v);   // tests force a rebase
    {   // one voice: the preamble, the operator's text and what a server asked for, then the harness's side of retrieval
        dd::know::PromptComposer composer;
        composer.section(system_prompt);
        dd::know::PromptComposer::Contract contract;
        contract.memory = maze->configured();
        contract.skills = skill_cat != nullptr;
        composer.section(dd::know::PromptComposer::retrieval_contract(contract));
        composer.section(dd::know::PromptComposer::catalogue_map(router.map()));
        if (std::ifstream bf(home.root / "brief.md", std::ios::binary); bf) {   // the operator's own standing notes, if there are any
            std::string brief((std::istreambuf_iterator<char>(bf)), std::istreambuf_iterator<char>());
            if (brief.size() > 3200) brief.resize(3200);
            composer.section(brief);
        }
        system_prompt = composer.str();
    }
    const bool soak = maze->configured() && !std::getenv("DD_NO_SOAK");   // test runs: recall, never write
    dd::context::TapeEngine engine(ec, system_prompt, ep, maze, soak ? &soaker : nullptr, deck, llm,
                                   work + "/spill", &router);
    router.set_ratio([&engine] { return engine.ratio(); });   // notes are counted in the server's tokens
    router.cancelled = [&agent] { return agent.interrupt_requested.load(); };
    router.on_log = [path = (home.logs() / "knowledge.jsonl").string()](const std::string& line) {   // what was offered, and what was opened
        std::ofstream(path, std::ios::app) << line << "\n";
    };
    // rules that cost nothing until a pattern fires: ./rules/*.md and the maze's rule: rows
    dd::know::Tripwires tripwires;
    dd::know::TripwireGate gate(tripwires);
    if (!cfg.has("knowledge.rules") || cfg.get_bool("knowledge.rules")) {
        tripwires.load_dir(home.rules());
        if (maze->up()) tripwires.load_maze(*maze);
    }
    tripwires.set_clock([&router] { return router.turn(); });
    gate.on_rule = [&router](const std::string& r) { router.note_tripwire(r); };
    gate.on_fire = [&scene](const std::string& n) { scene.note(n); };
    engine.on_note = [&scene](const std::string& n) { scene.note(n); };
    engine.on_recall = [&scene](bool on) { scene.recall(on); };
    engine.on_prefetch = [&scene](const dd::context::Prefetch& p, const std::string& summary) {
        if (p.shown.empty()) {
            scene.note("◈ " + summary);
            return;
        }
        dd::ui::RecallView v;
        v.meta = summary;
        v.skill = p.skill;
        for (const auto& [kind, h] : p.shown) v.rows.push_back(memory_row(kind, h));
        scene.recalled(v);
    };
    engine.on_stats = [&scene](const dd::context::TapeStats& t) {
        dd::ui::TapeView v;
        v.on = true;
        v.epoch = t.epoch;
        v.history = static_cast<long>(t.history);
        v.tape = static_cast<long>(t.tape);
        v.tokens = t.tokens;
        v.rebase_at = t.rebase_at;
        v.prompt = t.prompt;
        v.reused = t.reused;
        v.level = t.level;
        v.prep = t.prep;
        v.server = t.server;
        v.visible = t.visible;
        v.deferred = t.deferred;
        v.main_slot = t.main_slot;
        v.maze = t.maze;
        v.soaked = t.soaked;
        v.soak_queue = t.soak_queue;
        v.spooled = t.spooled;
        v.ledger = static_cast<long>(t.ledger);
        v.edges = static_cast<long>(t.edges);
        scene.set_tape(v);
    };
    llm.on_stats = [&scene, &engine](const dd::agent::CallStats& st) {
        dd::ui::LlmStats u;
        u.latency_ms = st.latency_ms;
        u.gen_tps = st.gen_tps;
        u.pp_tps = st.pp_tps;
        u.prompt_tokens = st.prompt_tokens;
        u.completion_tokens = st.completion_tokens;
        u.cached_tokens = st.cached_tokens;
        scene.llm_end(u);
        engine.on_call(st);
        if (const char* log = std::getenv("DD_TAPE_LOG")) {   // one line per call: is the prefix reused?
            const auto t = engine.stats();
            if (std::FILE* f = std::fopen(log, "a")) {
                std::fprintf(f, "{\"epoch\":%d,\"tape\":%zu,\"history\":%zu,\"prompt\":%ld,\"cached\":%ld,"
                                "\"completion\":%ld,\"ms\":%.0f,\"level\":\"%s\"}\n",
                             t.epoch, t.tape, t.history, st.prompt_tokens, st.cached_tokens, st.completion_tokens,
                             st.latency_ms, t.level.c_str());
                std::fclose(f);
            }
        }
    };

    agent.model = model;
    agent.max_iterations = max_iterations;
    agent.quiet_mode = true;
    agent.system_prompt = system_prompt;
    agent.registry = &reg;
    agent.llm = &llm;
    agent.context = &engine;
    agent.gate = &gate;

    const int tool_count = static_cast<int>(reg.get_all_tool_names().size());
    {
        dd::ui::Info info;
        info.model = model;
        info.endpoint = base_url;
        info.effort = effort_label(effort.load());
        info.tools = tool_count;
        info.max_turns = max_iterations;
        info.n_ctx = ep.info().n_ctx;
        info.sources = sources;
        scene.set_info(info);
    }
    for (const auto& e : boot_errors) scene.error(e);
    if (engine.on_stats) engine.on_stats(engine.stats());
    if (maze_url.empty()) scene.note("no mazemaker server: the tape forgets what ages out and nothing brings it back");
    else if (!maze->up()) scene.error("mazemaker at " + maze_url + " is not answering: rebases cannot recall, soaks spool to " + work);

    std::function<void()> flush_inline;
    agent.step_hook = [&](const dd::core::Message& m) {
        if (m.role == "tool") {
            scene.tool_result(m.tool_call_id, m.content);
        } else if (m.role == "assistant") {
            if (!m.content.empty() && !llm.streamed) scene.say(m.content);
            for (const auto& tc : m.tool_calls) scene.tool_call(tc.id, tc.name, tc.arguments);
        }
        if (flush_inline) flush_inline();
    };

    // ---- `-- msg` or piped stdin: cards printed in order ----
    if (!interactive) {
        int cols = 100, rows = 24;
        if (to_tty) term::size(cols, rows);
        flush_inline = [&] {
            const std::string s = scene.drain_inline(cols, color, truecolor);
            if (!s.empty()) { std::fwrite(s.data(), 1, s.size(), stdout); std::fflush(stdout); }
        };
        {
            const auto& th = scene.theme();
            dd::ui::Canvas c(std::clamp(cols, 40, dd::ui::Scene::kCardMaxW), 2);
            const int n = c.gradient(0, 0, "⟁ DAEDALUS", th.border, th.title, th.gold, dd::ui::kBold);
            c.text(n + 1, 0, "· " + model + " · " + std::to_string(tool_count) + " tools", dd::ui::Style{th.muted});
            const std::string s = c.to_ansi(color, truecolor);
            std::fwrite(s.data(), 1, s.size(), stdout);
        }
        auto turn = [&](const std::string& msg) {
            scene.user(msg);
            scene.turn_begin();
            flush_inline();
            agent.interrupt_requested = false;
            try { agent.run(msg); }
            catch (const std::exception& ex) { scene.error(std::string("agent: ") + ex.what()); }
            scene.turn_end();
            flush_inline();
        };
        if (!msg_words.empty()) {
            std::string user_message;
            for (size_t i = 0; i < msg_words.size(); ++i) user_message += (i ? " " : "") + msg_words[i];
            turn(user_message);
        } else {
            flush_inline();
            std::string line;
            while (std::getline(std::cin, line)) {
                line = trim_ws(line);
                if (line.empty()) continue;
                if (line == "/quit" || line == "/exit" || line == ":q") break;
                turn(line);
            }
        }
        return 0;
    }

    // ---- interactive: keys + frames on main, agent.run on a worker ----
    const char* mouse_env = std::getenv("DD_TUI_MOUSE");
    const bool mouse = !(mouse_env && std::strcmp(mouse_env, "0") == 0);
    const int saved_err = term::hide_stderr(home.logs().string());
    if (!term::enter(mouse)) {
        term::show_stderr(saved_err);
        std::cerr << "dd_tui: cannot put the terminal in raw mode\n";
        return 1;
    }

    std::atomic<bool> running{false};
    std::thread worker;
    std::string queued;
    bool quit = false;
    int cols = 80, rows = 24;
    Editor ed;
    KeyReader keys;

    auto interrupt = [&] {
        if (!running.load()) return;
        if (!agent.interrupt_requested.exchange(true)) scene.note("interrupting…");
    };

    auto start_turn = [&](const std::string& prompt) {
        if (worker.joinable()) worker.join();
        scene.user(prompt);
        scene.turn_begin();
        scene.scroll_end();
        agent.interrupt_requested = false;
        running = true;
        worker = std::thread([&, prompt] {
            try {
                agent.run(prompt);
            } catch (const std::exception& ex) {
                if (!agent.interrupt_requested.load()) scene.error(ex.what());
            }
            if (agent.interrupt_requested.load()) scene.note("⏹ interrupted");
            scene.turn_end();
            running = false;
        });
    };

    auto effort_cmd = [&](const std::string& arg) {
        std::string levels;
        for (const auto& e : efforts) levels += (levels.empty() ? "" : " ") + e;
        if (arg.empty()) {
            scene.note("effort " + effort_label(effort.load()) + " · this model: " +
                       (levels.empty() ? std::string("no reasoning switch in its template") : levels));
            return;
        }
        const int i = effort_index(arg);
        if (i < 0) {
            scene.note("unknown effort '" + arg + "' · off low medium high xhigh max ultra");
        } else if (!offered(i)) {
            scene.note("this model's template has no '" + std::string(kEfforts[i]) + "' (the server would answer 500) · it offers: " +
                       (levels.empty() ? std::string("nothing") : levels));
        } else {
            effort = i;
            scene.set_effort(kEfforts[i]);
            // the template writes the effort into the system block: the next call reads the prompt again
            scene.note(std::string("effort → ") + kEfforts[i] + (running.load() ? " from the next call" : "") +
                       " · the template puts it in the system block, so that call re-reads the whole prompt once");
        }
    };

    auto submit = [&](std::string p) {
        p = trim_ws(p);
        if (p.empty()) return;
        if (is_command(p)) {
            const std::string cmd = p.substr(0, p.find(' '));
            if (cmd == "/quit" || cmd == "/exit") {
                quit = true;
            } else if (cmd == "/help") {
                scene.note("enter send · esc / ctrl-c interrupt · ↑↓ history · ctrl-←→ words · pgup/pgdn or wheel scroll · "
                           "ctrl-l redraw · tab completes\n/effort off|low|…  /reasoning <effort>|show|full|hide  /context  /rebase  "
                           "/tools  /turns N  /clear (new session)  /interrupt  /quit");
            } else if (cmd == "/tools") {
                std::string all;
                for (const auto& n : reg.get_all_tool_names()) all += (all.empty() ? "" : "  ") + n;
                scene.note(std::to_string(tool_count) + " tools: " + all);
            } else if (cmd == "/effort" || cmd == "/reasoning") {
                const std::string arg = trim_ws(p.substr(cmd.size()));
                using dd::ui::ReasonView;
                if (cmd == "/reasoning" && (arg == "show" || arg == "tail" || arg == "full" || arg == "hide")) {
                    const ReasonView v = arg == "full" ? ReasonView::Full : arg == "hide" ? ReasonView::Hide : ReasonView::Tail;
                    scene.set_reason_view(v);
                    scene.note(std::string("reasoning ") + (v == ReasonView::Full ? "shown in full" : v == ReasonView::Hide ? "hidden" : "shown as a live tail"));
                } else {
                    effort_cmd(arg);
                }
            } else if (cmd == "/clear" || cmd == "/new") {
                if (running.load()) {
                    scene.note(cmd + " applies between turns");
                } else {
                    engine.reset();
                    scene.clear();
                    scene.note("new session " + engine.session().id() + " · the maze still has the last one");
                }
            } else if (cmd == "/context") {
                if (!running.load()) engine.sync();
                const auto t = engine.stats();
                const int reuse = t.prompt > 0 ? static_cast<int>(100.0 * static_cast<double>(t.reused) / static_cast<double>(t.prompt) + 0.5) : -1;
                scene.note("session " + t.session + " · epoch " + std::to_string(t.epoch) + " · history " +
                           std::to_string(t.history) + " msgs, tape " + std::to_string(t.tape) + "\nwindow " +
                           std::to_string(t.tokens) + " tokens (" + t.level + ") · rebase at " + std::to_string(t.rebase_at) +
                           " · prewarm at " + std::to_string(t.high_at) + (t.prep.empty() ? "" : " · next epoch " + t.prep) +
                           "\nlast call reused " + (reuse >= 0 ? std::to_string(reuse) + "% of " + std::to_string(t.prompt) : std::string("—")) +
                           " · tools " + std::to_string(t.visible) + " in window, " + std::to_string(t.deferred) +
                           " deferred · server " + t.server + ", main slot " + std::to_string(t.main_slot) +
                           "\nmaze " + (t.maze ? "up" : "down") + " · " + std::to_string(t.soaked) + " soaked, " +
                           std::to_string(t.soak_queue) + " queued, " + std::to_string(t.spooled) + " spooled · ledger " +
                           std::to_string(t.ledger) + " nodes, " + std::to_string(t.edges) + " edges · " +
                           std::to_string(t.skills) + " skills indexed" + (t.recall.empty() ? "" : "\nlast recall: " + t.recall));
            } else if (cmd == "/resume") {
                const std::string arg = trim_ws(p.substr(cmd.size()));
                if (running.load()) {
                    scene.note("/resume applies between turns");
                } else if (arg.empty()) {
                    std::string list;
                    for (const auto& r : engine.recent_sessions("dd-", 8))
                        list += "\n  " + r.id + " · " + std::to_string(r.turns) + " turns · " + r.last;
                    scene.note(list.empty() ? std::string("no dd sessions in the maze yet")
                                            : "sessions in the maze (/resume <id>, any auto:turn session works):" + list);
                } else {
                    std::string why, sid = arg;
                    if (arg == "last")   // the newest session that is not this one
                        for (const auto& r : engine.recent_sessions("dd-", 3))
                            if (r.id != engine.session().id()) {
                                sid = r.id;
                                break;
                            }
                    scene.clear();
                    if (!engine.resume(sid, &why)) scene.error("resume " + sid + ": " + why);
                }
            } else if (cmd == "/rebase") {
                if (running.load()) scene.note("/rebase applies between turns");
                else engine.rebase_now();
            } else if (cmd == "/interrupt") {
                interrupt();
            } else if (cmd == "/turns") {
                if (running.load()) {
                    scene.note("/turns applies between turns");
                } else {
                    try { agent.max_iterations = std::max(1, std::stoi(p.substr(6))); } catch (...) {}
                    scene.set_max_turns(agent.max_iterations);
                    scene.note("max turns = " + std::to_string(agent.max_iterations));
                }
            } else {
                scene.note("unknown command " + cmd + " · /help");
            }
            return;
        }
        if (running.load()) {
            queued = p;
            scene.note("queued — sends when this turn ends");
            return;
        }
        start_turn(p);
    };

    auto handle = [&](const Key& k) {
        switch (k.k) {
        case K::Char: case K::Paste: ed.insert(k.text); break;
        case K::Enter: submit(dd::ui::to_utf8(ed.take())); break;
        case K::Backspace: ed.backspace(); break;
        case K::Delete: ed.del(); break;
        case K::Left: if (ed.cur) --ed.cur; break;
        case K::Right: if (ed.cur < ed.buf.size()) ++ed.cur; break;
        case K::WordLeft: ed.word_left(); break;
        case K::WordRight: ed.word_right(); break;
        case K::Up: ed.recall(-1); break;
        case K::Down: ed.recall(+1); break;
        case K::Home: ed.cur = 0; break;
        case K::End: ed.cur = ed.buf.size(); break;
        case K::PgUp: scene.scroll(std::max(1, rows - 8)); break;
        case K::PgDn: scene.scroll(-std::max(1, rows - 8)); break;
        case K::WheelUp: scene.scroll(3); break;
        case K::WheelDown: scene.scroll(-3); break;
        case K::Tab: {
            const std::string typed = dd::ui::to_utf8(ed.buf);
            if (is_command(typed) && typed.find(' ') == std::string::npos) {
                const auto m = command_matches(typed);
                if (m.size() == 1) {
                    const bool takes_arg = m[0] == "/turns" || m[0] == "/effort" || m[0] == "/reasoning";
                    ed.buf = dd::ui::from_utf8(m[0] + (takes_arg ? " " : ""));
                    ed.cur = ed.buf.size();
                }
            } else if (const auto m = arg_matches(typed, efforts); m.size() == 1) {
                ed.buf = dd::ui::from_utf8(typed.substr(0, typed.find(' ') + 1) + m[0]);
                ed.cur = ed.buf.size();
            }
            break;
        }
        case K::Esc: if (running.load()) interrupt(); else ed.clear(); break;
        case K::CtrlC:
            if (running.load()) interrupt();
            else if (!ed.buf.empty()) ed.clear();
            else quit = true;
            break;
        case K::CtrlD:
            if (ed.buf.empty() && !running.load()) quit = true;
            else ed.del();
            break;
        case K::CtrlK: ed.buf.erase(ed.cur); break;
        case K::CtrlU: ed.buf.erase(0, ed.cur); ed.cur = 0; break;
        case K::CtrlW: ed.kill_word(); break;
        case K::CtrlL: scene.redraw(); break;
        }
    };

    char rbuf[4096];
    while (!quit) {
        pollfd pfd{STDIN_FILENO, POLLIN, 0};
        // 20 fps while the spinner turns, ~8 fps idle (frames with no change write nothing)
        const int pr = ::poll(&pfd, 1, running.load() ? 50 : 120);
        if (pr > 0) {
            const ssize_t n = ::read(STDIN_FILENO, rbuf, sizeof rbuf);
            if (n > 0) {
                for (const Key& k : keys.feed(rbuf, static_cast<size_t>(n))) handle(k);
            } else if (pfd.revents & (POLLHUP | POLLERR)) {
                quit = true;   // the terminal went away
            }
        }
        if (!running.load() && worker.joinable()) worker.join();
        if (!running.load() && !queued.empty() && !quit) {
            std::string q;
            q.swap(queued);
            start_turn(q);
        }
        term::size(cols, rows);
        std::string hint;
        if (const std::string typed = dd::ui::to_utf8(ed.buf); is_command(typed) && typed.find(' ') == std::string::npos) {
            for (const auto& m : command_matches(typed)) hint += (hint.empty() ? "" : "  ") + m;
            if (hint.empty()) hint = "no such command";
        } else if (typed.rfind("/effort ", 0) == 0 || typed.rfind("/reasoning ", 0) == 0) {
            for (const auto& m : arg_matches(typed, efforts)) hint += (hint.empty() ? "" : "  ") + m;
            if (hint.empty()) hint = efforts.empty() ? "this model has no reasoning switch" : "not offered by this model";
        }
        scene.set_input(ed.buf, ed.cur, hint);
        term::put(scene.present(cols, rows, truecolor));
    }

    if (running.load()) {   // give the loop a moment to stop at a step boundary
        agent.interrupt_requested = true;
        for (int i = 0; i < 40 && running.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    term::leave();
    term::show_stderr(saved_err);
    {
        const auto& th = scene.theme();
        dd::ui::Canvas c(std::max(1, dd::ui::text_width(th.goodbye) + 1), 1);
        c.text(0, 0, th.goodbye, dd::ui::Style{th.accent, dd::ui::kNone, dd::ui::kItalic});
        const std::string s = c.to_ansi(color, truecolor);
        std::fwrite(s.data(), 1, s.size(), stdout);
        std::fflush(stdout);
    }
    if (running.load()) std::_Exit(0);   // a tool still holds the worker
    if (worker.joinable()) worker.join();
    return 0;
}
