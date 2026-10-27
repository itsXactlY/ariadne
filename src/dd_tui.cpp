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
//   build/dd_tui --help | --version
//   env: DD_HOME, DD_BASE_URL, DD_MODEL, DD_API_KEY, DD_MAX_TURNS, DD_TUI_COLS, DD_TUI_ROWS,
//        DD_TUI_MOUSE=0 (keep the terminal's own mouse), NO_COLOR
//   commands: /help /tools /turns N /effort L /reasoning L|show|full|hide /verbose off|normal|full (bare toggles) /plan /context /rebase /resume /clear /learn /interrupt /mcp /<server> /quit

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
#include "dd/tools/fs_tools.hpp"
#include "dd/tools/tool_output_limits.hpp"
#include "dd/tools/patch_apply.hpp"
#include "dd/tools/skill_provenance.hpp"
#include "dd/tools/todo_tool.hpp"
#include "dd/tools/mcp.hpp"
#include "dd/tools/shell.hpp"
#include "dd/tools/registry.hpp"
#include "dd/tools/tool.hpp"
#include "dd/tools/tool_search.hpp"
#include "dd/ui.hpp"
#include "dd/ui_keys.hpp"
#include "dd/ui_simd.hpp"

#include <atomic>
#include <cctype>
#include <cerrno>
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
#include <deque>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace ddcore = ::dd::core;
using namespace dd::core;
using dd::tools::ToolEntry;
using dd::tools::MCPClient;
using dd::ui::Editor;
using dd::ui::K;
using dd::ui::Key;
using dd::ui::KeyReader;

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

// ---- file and shell tools: dd/tools/fs_tools.hpp, dd/tools/shell.hpp ----
static dd::know::Resolver* g_resolver = nullptr;

static dd::tools::FsLimits g_fs_limits;      // resolved from tool_output at startup
static size_t g_shell_max_output = 8000;      // ditto; the shipped default until the config says otherwise   // skill://name, mem://id: one verb reads everything the catalogues hold

static std::string h_read_file(const Json& args) {
    if (const Json* p = json_get(args, "path"); p && p->type == Json::Str && g_resolver)
        if (auto internal = g_resolver->read(p->str)) return *internal;
    return dd::tools::fs_read(args, g_fs_limits);
}
static std::string h_write_file(const Json& args) { return dd::tools::fs_write(args); }
static std::string h_list_dir(const Json& args) { return dd::tools::fs_list(args, g_fs_limits); }

// the agent's interrupt flag: Esc, and a SIGINT/SIGTERM/SIGHUP, reach a running shell command through it
static std::atomic<std::atomic<bool>*> g_cancel{nullptr};
static std::atomic<int> g_sig{0};

static void on_term(int sig) {   // async-signal-safe: two atomic stores, the loops do the rest
    g_sig.store(sig, std::memory_order_relaxed);
    if (std::atomic<bool>* c = g_cancel.load(std::memory_order_relaxed)) c->store(true, std::memory_order_relaxed);
}

static void install_term_handlers() {   // no SA_RESTART: a blocking poll/read wakes and sees g_sig
    struct sigaction sa {};
    sa.sa_handler = on_term;
    sigemptyset(&sa.sa_mask);
    for (int s : {SIGINT, SIGTERM, SIGHUP}) sigaction(s, &sa, nullptr);
}

static std::string run_terminal(const Json& args, const ddcore::Config& cfg) {
    const Json* c = json_get(args, "cmd");
    if (!c || c->type != Json::Str || c->str.empty()) return jerr("cmd required");
    dd::tools::ShellPolicy policy;
    if (const Json* d = cfg.get_list("command_denylist"); d && d->type == Json::Arr)
        for (const auto& sp : d->arr)
            if (sp && sp->type == Json::Str) policy.extra_deny.push_back(sp->str);
    dd::tools::ShellOptions opt;
    opt.max_output = g_shell_max_output;
    opt.cancel = g_cancel.load(std::memory_order_relaxed);
    return dd::tools::run_shell(c->str, opt, policy);
}

// cut at n bytes, never inside a UTF-8 sequence
static void utf8_trunc(std::string& s, size_t n) {
    if (s.size() <= n) return;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    s.resize(n);
}

static const char* const kVersion = "0.1.0";

static void usage(FILE* f) {
    std::fputs(
        "usage: dd_tui [--home DIR | --config FILE | FILE] [-- message words...]\n"
        "       dd_tui --init [--from FILE] [--rules]     write config.yaml and the folders beside the binary\n"
        "       dd_tui --help | --version\n"
        "\n"
        "  --home DIR      run from another folder (config.yaml, skills/, rules/, state/, logs/)\n"
        "  --config FILE   an explicit config; its folder is the home (a bare path argument does the same)\n"
        "  --init          write config.yaml (the embedded reference, or a copy of --from FILE) and the folders\n"
        "  --rules         with --init: also install the starter tripwires\n"
        "  -- words...     one message, cards printed inline; with no words and a tty: the interactive screen\n"
        "\n"
        "env: DD_HOME DD_BASE_URL DD_MODEL DD_API_KEY DD_MAX_TURNS DD_TUI_COLS DD_TUI_ROWS DD_TUI_MOUSE=0 NO_COLOR\n",
        f);
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
    for (int s : {SIGQUIT, SIGSEGV, SIGABRT, SIGBUS, SIGFPE}) std::signal(s, on_fatal);   // INT/TERM/HUP: on_term, see main
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

static const char* const kCommands[] = {"/help", "/tools", "/turns", "/effort", "/reasoning", "/verbose", "/plan", "/context", "/rebase",
                                        "/resume", "/clear", "/new", "/learn", "/interrupt", "/quit",
                                        "/mcp"};   // every live MCP server adds its own "/<server>" at boot

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

// filled at boot with "/<server>" for every live MCP server, so a registered server
// completes as a slash command all by itself.
static std::vector<std::string> g_dynamic_commands;

static std::vector<std::string> command_matches(const std::string& typed) {
    std::vector<std::string> m;
    for (const char* c : kCommands)
        if (std::string(c).rfind(typed, 0) == 0) m.emplace_back(c);
    for (const auto& c : g_dynamic_commands)
        if (std::string(c).rfind(typed, 0) == 0) m.emplace_back(c);
    return m;
}

// "/effort x" / "/reasoning x" / "/verbose x": the arguments that fit the prefix x.
static std::vector<std::string> arg_matches(const std::string& typed, const std::vector<std::string>& efforts) {
    std::vector<std::string> m;
    const size_t sp = typed.find(' ');
    if (sp == std::string::npos || typed.find(' ', sp + 1) != std::string::npos) return m;
    const std::string cmd = typed.substr(0, sp), pre = typed.substr(sp + 1);
    std::vector<std::string> all;
    if (cmd == "/verbose") all = {"quiet", "normal", "full"};
    else if (cmd == "/effort") all = efforts;
    else if (cmd == "/reasoning") {
        all = efforts;
        all.insert(all.end(), {"show", "tail", "full", "hide"});
    } else return m;
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
        auto value = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "dd_tui: " << flag << " needs a value\n";
                usage(stderr);
                std::exit(2);
            }
            return argv[++i];
        };
        if (after_dd) msg_words.push_back(a);
        else if (a == "--") after_dd = true;
        else if (a == "--help" || a == "-h") { usage(stdout); return 0; }
        else if (a == "--version") { std::printf("dd_tui %s\n", kVersion); return 0; }
        else if (a == "--home") home_flag = value("--home");
        else if (a == "--config") config_flag = value("--config");
        else if (a == "--init") init = true;
        else if (a == "--rules") init_rules = true;
        else if (a == "--from") init_from = value("--from");
        else if (a.size() > 1 && a[0] == '-') { std::cerr << "dd_tui: unknown option " << a << "\n"; usage(stderr); return 2; }
        else config_flag = a;   // a bare path is the config, as it always was
    }

    // Config and state follow the folder dd was started from; the install is
    // where the binary sits. Never the Python Daedalus's ~/.daedalus.
    std::string why;
    const ddcore::Home home = ddcore::resolve_home(home_flag, config_flag, &why);
    if (home.root.empty()) { std::cerr << why << "\n"; return 2; }
    ddcore::set_daedalus_home_override(home.root.string());   // the ported modules agree on it
    const std::string config_path = config_flag.empty() ? home.config().string() : config_flag;

    // Config first: an install with no config.yaml runs on defaults with a warning,
    // not on another install's config. A home dd has never written in is not
    // created on a plain start — --init is the only path that may make one.
    std::error_code boot_ec;
    if (init) {
        std::filesystem::create_directories(home.root, boot_ec);
    } else if (!std::filesystem::exists(config_path, boot_ec) && !std::getenv("DD_BASE_URL")) {
        std::cerr << "dd_tui: no config at " << config_path << "\n"
                  << "  " << home.root.string() << " is the home dd picked and it has no config.yaml, so dd\n"
                  << "  has never been configured there. Start dd from the project folder that owns the\n"
                  << "  config.yaml, name one with --config FILE / --home DIR, or run dd_tui --init to\n"
                  << "  write one (dd_tui --init --from FILE copies an existing config, once). Nothing was\n"
                  << "  created.\n";
        return 2;   // the home was never configured: stop before it is built
    }
    if (!ddcore::ensure_home(home, &why)) {
        std::cerr << "dd_tui: " << why << "\n  pass --home DIR for a folder you can write\n";
        return 2;
    }
    if (init) {
        const bool wrote = ddcore::init_config(home, init_from, &why);
        std::cout << (wrote ? "wrote " : "kept ") << config_path
                  << (wrote || why.empty() ? "" : "  (" + why + ")") << "\n";
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

    ddcore::Config cfg{ddcore::Json{}};
    try { cfg = ddcore::load_config(config_path); }
    catch (const std::exception& ex) { std::cerr << "config: " << ex.what() << "\n"; }
    dd::know::configure(cfg);

    // The tool-output envelope, resolved once from the config this process loaded: run_terminal's
    // byte cap and read_file's line window come from `tool_output`. Absent values keep the numbers
    // the tools shipped before the section existed, so an unset config changes nothing.
    const auto tool_output = dd::tools::resolve_tool_output_from(cfg);
    g_fs_limits.max_lines = static_cast<size_t>(tool_output.at("max_lines"));
    g_fs_limits.max_line_length = static_cast<size_t>(tool_output.at("max_line_length"));
    g_fs_limits.max_read_bytes = static_cast<size_t>(tool_output.at("read_bytes"));
    g_shell_max_output = static_cast<size_t>(tool_output.at("max_bytes"));

    auto env_or = [](const char* k, const std::string& def) -> std::string {
        const char* v = std::getenv(k);
        return (v && *v) ? std::string(v) : def;
    };
    std::string base_url = env_or("DD_BASE_URL", cfg.get_string("model.base_url"));
    std::string model = env_or("DD_MODEL", cfg.get_string("model.name"));
    std::string api_key = env_or("DD_API_KEY", cfg.get_string("model.api_key"));
    long long mt = 0;
    if (const char* v = std::getenv("DD_MAX_TURNS"); v && *v) {
        char* end = nullptr;
        errno = 0;
        const long long n = std::strtoll(v, &end, 10);
        if (errno == 0 && end && *end == '\0' && n > 0) mt = n;
        else std::cerr << "dd_tui: DD_MAX_TURNS='" << v << "' is not a positive number: ignored\n";
    }
    if (mt <= 0) {
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
    {   // verbosity: config, then DD_VERBOSE, then /verbose at runtime
        using dd::ui::Verbosity;
        std::string v = cfg.has("display.verbose") ? cfg.get_string("display.verbose") : "normal";
        if (const char* e = std::getenv("DD_VERBOSE")) v = e;
        if (v == "off" || v == "quiet" || v == "minimal") scene.set_verbosity(Verbosity::Minimal);
        else if (v == "full" || v == "verbose") scene.set_verbosity(Verbosity::Verbose);
    }

    // A/B knob: names to leave out of the deck entirely (comma-separated). The prompt, the tape and
    // the binary stay identical, so the delta measures the tool and not the build.
    std::set<std::string> disabled_tools;
    if (const char* v = std::getenv("DD_DISABLE_TOOLS"); v && *v) {
        std::string tok;
        for (const char* c = v; ; ++c) {
            if (*c == ',' || *c == '\0') {
                if (!tok.empty()) disabled_tools.insert(tok);
                tok.clear();
                if (*c == '\0') break;
            } else if (*c != ' ') {
                tok.push_back(*c);
            }
        }
    }

    // ---- the registry; every handler reports its run to the scene ----
    dd::tools::ToolRegistry reg;
    dd::tools::TodoStore todos;              // one ledger per session, model-owned
    std::function<void()> publish_plan;      // bound below, once the agent exists
    auto add_tool = [&](ToolEntry e) {
        if (disabled_tools.count(e.name)) return;   // never registered, so never deckable
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
    { ToolEntry e; e.name = "write_file"; e.schema = dd::tools::fs_tool_schema({"path", "content"}, "Write text content to a file."); e.handler = h_write_file; add_tool(e); }
    { ToolEntry e; e.name = "list_dir"; e.schema = dd::tools::fs_tool_schema({"path"}, "List entries of a directory."); e.handler = h_list_dir; add_tool(e); }
    {
        ToolEntry e;
        e.name = "edit_file";
        e.schema = dd::tools::edit_tool_schema();
        e.handler = [&home](const Json& a) {
            dd::tools::ApplyOptions opt;
            opt.skills_root = home.skills().string();   // a learned skill the model edits is annotated
            const dd::tools::WriteOriginScope scope("model_edit");
            return dd::tools::fs_edit(a, opt);
        };
        add_tool(e);
    }
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
            e.schema = json_obj({{"description", jsp(json_str("Run a shell command. A guard refuses obviously destructive commands (sudo, recursive rm on broad paths, force-push, ...); it is not a sandbox. Returns exit code + stdout."))},
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
    struct CmdServ {                   // one live MCP server as a slash command
        std::string cmd, url;          // "/pulse", its endpoint
        std::vector<std::string> tools;  // the names it answers with: every wire name + bare alias
        std::set<std::string> wire;      // the wire names alone: a bare alias maps back to "<server>_<alias>"
    };
    std::vector<CmdServ> cmd_servs;
    auto find_cmd_serv = [&](const std::string& c) -> CmdServ* {
        for (auto& sv : cmd_servs)
            if (sv.cmd == c) return &sv;
        return nullptr;
    };
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
            if (const Json* hs = json_get(sv, "headers"); hs && hs->type == Json::Obj)
                for (const auto& [hk, hv] : hs->obj)
                    if (hv && hv->type == Json::Str) mcp->headers.emplace_back(hk, hv->str);   // ${ENV} is expanded per request
            try {
                // what to do with the server's own protocol text depends on whether it is the maze: decided after the loop
                if (const std::string instr = mcp->instructions(); !instr.empty()) {
                    const Json* ic = json_get(sv, "instructions");
                    server_notes.push_back({u->str, instr, ic && ic->type == Json::Str ? ic->str : std::string()});
                }
                Json tl = mcp->list_tools();
                std::size_t registered = 0;
                std::vector<std::string> registered_tools;   // feeds the server's own slash command
                std::set<std::string> wire_names;
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
                        wire_names.insert(tname);
                        registered_tools.push_back(tname);
                        if (const size_t d = tname.size(); srvname.size() + 1 < d &&
                            tname.rfind(srvname + "_", 0) == 0)
                            registered_tools.push_back(tname.substr(srvname.size() + 1));  // bare alias: pulse_search -> "search" under /pulse
                        ++registered;
                    }
                }
                sources.push_back(srvname + "  " + std::to_string(registered));
                {   // the server drives itself from the input line, unless the name is a built-in command
                    const std::string cmdname = "/" + srvname;
                    const bool taken = std::any_of(std::begin(kCommands), std::end(kCommands), [&](const char* c) { return cmdname == c; });
                    if (!taken) {
                        cmd_servs.push_back({cmdname, u->str, registered_tools, wire_names});
                        g_dynamic_commands.push_back(cmdname);
                    }
                }

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
        e.name = "skills_list";
        e.schema = dd::know::Resolver::skills_list_schema();
        e.handler = [&resolver](const Json& a) { return resolver.skills_list(a); };
        add_tool(e);
    }
    {
        ToolEntry e;
        e.name = "todo";
        const std::string desc = "Work ledger for a multi-step task. Write the plan before acting, update statuses as "
                                 "you go, keep one item active at a time. Provide `todos` to write the whole list (list "
                                 "position is priority order); omit `todos` to read it back. `merge: true` updates only "
                                 "the items named and keeps the rest. The list is re-attached to every model call until "
                                 "it is empty.";
        Json item = json_obj({{"id", jsp(json_obj({{"type", jsp(json_str("string"))}, {"description", jsp(json_str("short identifier you choose; keep it stable across merges"))}}))},
                              {"content", jsp(json_obj({{"type", jsp(json_str("string"))}, {"description", jsp(json_str("the step, one line"))}}))},
                              {"status", jsp(json_obj({{"type", jsp(json_str("string"))},
                                                       {"enum", jsp(json_arr({jsp(json_str("pending")), jsp(json_str("in_progress")), jsp(json_str("completed")), jsp(json_str("cancelled"))}))}}))}});
        Json props = json_obj({{"todos", jsp(json_obj({{"type", jsp(json_str("array"))}, {"items", jsp(std::move(item))},
                                                       {"description", jsp(json_str("the whole list in priority order; omit to read"))}}))},
                               {"merge", jsp(json_obj({{"type", jsp(json_str("boolean"))}, {"description", jsp(json_str("true: update the named items, keep the rest"))}}))}});
        e.schema = json_obj({{"description", jsp(json_str(desc))},
                             {"parameters", jsp(json_obj({{"type", jsp(json_str("object"))}, {"properties", jsp(std::move(props))}}))}});
        e.handler = [&todos, &publish_plan](const Json& a) {
            const Json* t = json_get(a, "todos");
            const Json* m = json_get(a, "merge");
            const bool merge = m && m->type == Json::Bool && m->b;
            const std::string r = dd::tools::todo_tool(t ? *t : Json{}, merge, todos);
            if (publish_plan) publish_plan();
            return r;
        };
        add_tool(e);
    }

    std::set<std::string> core_set{"read_file", "write_file", "edit_file", "list_dir", "run_terminal", "find", "skill_load", "skills_list", "todo"};
    for (const auto& n : disabled_tools) core_set.erase(n);
    deck.arrange(core_set);
    tool_cat->refresh();   // local and instant: searchable at once, and the one catalogue that reads the deck
    // the router learns what was worth offering (priors, terms for the cards that were found by other words); the overlay
    // must be in place before the first refresh, the service itself starts after the refreshers (it is stopped first)
    const bool learn_on = !cfg.has("learn.enabled") || cfg.get_bool("learn.enabled");
    auto learn_overlay = std::make_shared<dd::learn::LearnOverlay>();
    if (learn_on && skill_cat) skill_cat->set_overlay(learn_overlay);
    if (learn_on && memory_cat) memory_cat->set_overlay(learn_overlay);   // the maze's rows are curated too, not just the skills
    if (memory_cat) memory_cat->preload();   // the cache is local too: searchable before any pod call, with the overlay already in place
    if (learn_on) router.set_overlay(learn_overlay);   // the router reads demand from it: offered vs opened, the guard's evidence
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
        lo.author = cfg.has("learn.author") && cfg.get_bool("learn.author");   // drafting skills is opt-in
        lo.govern = !cfg.has("learn.govern") || cfg.get_bool("learn.govern");   // governing them is not: quiet by default
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
        // An off learning loop is not a silent condition: priors, drafts, the lifecycle and the router's demand
        // guard all read the journal, so say why it is off rather than letting the router guess from empty demand.
        else if (!lerr.empty())
            boot_errors.push_back("learning is off: " + lerr +
                                  " — no fold, so the router judges demand by this run's own offers only");
    }
    const std::string work = home.state().string();   // spill and the soak spool: beside the binary, not the launch dir
    dd::context::Soaker soaker(maze, work + "/soak-spool.jsonl");

    // ---- the transport ----
    dd::agent::Agent agent;
    g_cancel.store(&agent.interrupt_requested);
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
        contract.learn = learner != nullptr;
        if (!cfg.has("agent.contract") || cfg.get_bool("agent.contract"))
            composer.section(dd::know::PromptComposer::workflow_contract(contract));
        composer.section(dd::know::PromptComposer::retrieval_contract(contract));
        composer.section(dd::know::PromptComposer::catalogue_map(router.map()));
        if (std::ifstream bf(home.root / "brief.md", std::ios::binary); bf) {   // the operator's own standing notes, if there are any
            std::string brief((std::istreambuf_iterator<char>(bf)), std::istreambuf_iterator<char>());
            utf8_trunc(brief, 3200);
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

    // The plan reaches the model as an ephemeral prompt, never through the tape: the
    // cached prefix stays byte-identical, only the tail moves. An empty ledger empties
    // the prompt, so the weight disappears the moment the work is done.
    publish_plan = [&todos, &scene, &agent] {
        const auto inj = todos.format_for_injection();
        agent.ephemeral_system_prompt = inj ? *inj : "";
        dd::ui::PlanView p;
        for (const auto& it : todos.items) {
            dd::ui::PlanView::Marker m = dd::ui::PlanView::Pending;
            if (it.status == "in_progress") m = dd::ui::PlanView::Active;
            else if (it.status == "completed") m = dd::ui::PlanView::Done;
            else if (it.status == "cancelled") m = dd::ui::PlanView::Dropped;
            if (m == dd::ui::PlanView::Pending || m == dd::ui::PlanView::Active) ++p.open;
            ++p.total;
            p.items.emplace_back(m, it.content);
        }
        scene.set_plan(p);
    };

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
        install_term_handlers();
        bool failed = false;   // a run that threw makes the exit status non-zero
        auto turn = [&](const std::string& msg) {
            scene.user(msg);
            scene.turn_begin();
            flush_inline();
            agent.interrupt_requested = false;
            try { agent.run(msg); }
            catch (const std::exception& ex) {
                if (!g_sig.load()) scene.error(std::string("agent: ") + ex.what());
                failed = true;
            }
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
            while (!g_sig.load() && std::getline(std::cin, line)) {
                line = trim_ws(line);
                if (line.empty()) continue;
                if (line == "/quit" || line == "/exit" || line == ":q") break;
                turn(line);
            }
        }
        dd::tools::shell_kill_all();
        if (const int sig = g_sig.load()) return 128 + sig;
        return failed ? 1 : 0;
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

    install_term_handlers();   // after raw mode: INT/TERM/HUP end the loop below, not the process mid-frame

    std::atomic<bool> running{false};
    std::thread worker;
    // Typed while a turn or a command runs. Two consumers, so it is locked: the
    // agent thread pulls it at each round boundary (steering, folded into the turn
    // that is already running), the idle loop sends what is left as a turn once
    // nothing runs. Both drain, neither drops.
    std::mutex qmu;
    std::deque<std::string> queued;
    bool quit = false;
    int cols = 80, rows = 24;
    Editor ed;
    KeyReader keys;

    // A slow command (a recall, a rebase, a server call) runs here so the screen and the keys stay live. One at a time,
    // and no turn starts beside it: the engine belongs to whichever runs.
    std::thread cmd_thread;
    std::atomic<bool> cmd_busy{false};
    std::string cmd_label;   // the UI thread's
    auto run_cmd = [&](const std::string& label, std::function<void()> fn) {
        if (cmd_busy.load()) {
            scene.note("still running: " + cmd_label);
            return;
        }
        if (cmd_thread.joinable()) cmd_thread.join();   // finished: cmd_busy is already false
        cmd_label = label;
        cmd_busy = true;
        scene.note("… " + label);
        cmd_thread = std::thread([&scene, &cmd_busy, label, fn = std::move(fn)] {
            try {
                fn();
            } catch (const std::exception& ex) {
                scene.error(label + ": " + ex.what());
            }
            cmd_busy = false;
        });
    };

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
        publish_plan();   // fresh ledger for this turn; clears a stale one
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

    // Mid-turn steering: the agent thread pulls what was typed since the last round
    // and the loop puts it in the next call. The transcript shows it as input the
    // turn received, at the boundary it entered — not as if it had been typed before
    // the turn began.
    agent.steer_hook = [&] {
        std::vector<std::string> typed;
        {
            std::lock_guard<std::mutex> lk(qmu);
            typed.assign(queued.begin(), queued.end());   // a deque is not a vector: copy out, then clear
            queued.clear();
        }
        for (const auto& t : typed) scene.user(t);
        if (!typed.empty())
            scene.note("◈ steered · " + std::to_string(typed.size()) +
                       (typed.size() == 1 ? " prompt entered this turn" : " prompts entered this turn"));
        return typed;
    };

    std::function<void()> learn_pin;   // /learn: null while learning is off
    if (learner) learn_pin = [&learner] { learner->pin(); };
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

    // ---- /<server>: every live MCP server drives itself straight from the input line ----
    // A server that answered at boot has "/<name>" in autocomplete and runs through here: no model turn needed.
    // Runs on the command thread (a call can take seconds); anything it throws is reported by run_cmd.
    auto mcp_invoke = [&](const CmdServ& sv, const std::string& tool, const std::string& args) {
        MCPClient* m = nullptr;
        for (const auto& c : mcps)
            if (c->url == sv.url) { m = c.get(); break; }
        if (!m) {
            scene.error("server " + sv.cmd + " went down since boot");
            return;
        }
        if (tool.empty()) {
            const Json tl = m->list_tools();
            std::string names;
            int n = 0;
            if (tl.type == Json::Arr)
                for (const auto& tsp : tl.arr)
                    if (tsp && tsp->type == Json::Obj)
                        if (const Json* nm = json_get(*tsp, "name"); nm && nm->type == Json::Str) {
                            names += (names.empty() ? "" : ", ") + nm->str;
                            ++n;
                        }
            scene.note(sv.cmd + " · " + std::to_string(n) + " tools:\n  " + names);
            return;
        }
        const std::string wire = sv.wire.count(tool) ? tool : sv.cmd.substr(1) + "_" + tool;   // a bare alias -> its wire name
        if (std::getenv("DD_NO_SOAK") && maze_write(wire)) {
            scene.error(sv.cmd + " " + tool + ": DD_NO_SOAK: this run writes nothing to the maze");
            return;
        }
        bool repaired = false;
        Json ja = dd::core::json_parse_args(args.empty() ? "{}" : args, repaired);
        if (ja.type != dd::core::Json::Obj && !args.empty())
            ja = json_obj({{"query", jsp(json_str(args))}});  // a bare word: the usual one-arg shape
        if (ja.type != dd::core::Json::Obj) ja = json_obj();
        std::string shown = m->call(wire, ja);
        bool failed = false;
        try {   // a failed call comes back as {"error": ...}
            const Json j = json_parse(shown);
            if (j.type == Json::Obj)
                if (const Json* e = json_get(j, "error")) {
                    failed = true;
                    shown = e->type == Json::Str ? e->str : json_dump(*e);
                }
        } catch (...) {
        }
        if (shown.empty()) shown = "(no output)";
        if (shown.size() > 6000) {
            utf8_trunc(shown, 6000);
            shown += " …[truncated]";
        }
        if (failed) scene.error(sv.cmd + " " + tool + ": " + shown);
        else scene.note(shown);
    };
    auto invoke = [&](const CmdServ& sv, const std::string& tool, const std::string& args) {
        run_cmd(sv.cmd, [&, sv, tool, args] { mcp_invoke(sv, tool, args); });
    };

    auto submit = [&](std::string p) {
        p = trim_ws(p);
        if (p.empty()) return;
        if (is_command(p)) {
            const std::string cmd = p.substr(0, p.find(' '));
            const CmdServ* serv = find_cmd_serv(cmd);
            // every live MCP server registered its own command at boot; also take a unique
            // prefix of one, so the typed verb works while autocomplete lags.
            if (!serv && cmd.size() > 1) {
                const CmdServ* hit = nullptr;
                int n = 0;
                for (const auto& sv : cmd_servs)
                    if (sv.cmd.rfind(cmd, 0) == 0) { hit = &sv; ++n; }
                if (n == 1) serv = hit;
            }
            if (cmd == "/quit" || cmd == "/exit") {
                quit = true;
            } else if (cmd == "/help") {
                scene.note("enter send · esc / ctrl-c interrupt · ↑↓ history · ctrl-←→ words · pgup/pgdn or wheel scroll · "
                           "ctrl-l redraw · tab completes\n/effort off|low|…  /reasoning <effort>|show|full|hide  /verbose quiet|normal|full  /context  /rebase  "
                           "/resume  /tools  /turns N  /plan  /clear (new session)  /learn  /mcp  /<server>  /interrupt  /quit");
            } else if (cmd == "/learn") {
                if (learn_pin) run_cmd("learn", [&] {
                    learn_pin();
                    scene.note("learn: the recent work becomes a skill when this turn ends");
                });
                else scene.note("learning is off");
            } else if (cmd == "/tools") {
                std::string all;
                for (const auto& n : reg.get_all_tool_names()) all += (all.empty() ? "" : "  ") + n;
                scene.note(std::to_string(tool_count) + " tools: " + all);
            } else if (cmd == "/verbose") {
                const std::string arg = trim_ws(p.substr(cmd.size()));
                using dd::ui::Verbosity;
                Verbosity v = Verbosity::Normal;
                if (arg == "off" || arg == "quiet" || arg == "minimal") v = Verbosity::Minimal;
                else if (arg == "full" || arg == "verbose") v = Verbosity::Verbose;
                else if (arg.empty()) v = scene.verbosity() == Verbosity::Verbose ? Verbosity::Normal : Verbosity::Verbose;
                scene.set_verbosity(v);
                scene.note(v == Verbosity::Minimal ? "verbose off — cards fold to a glance"
                           : v == Verbosity::Verbose ? "verbose on — full arguments, results, reasoning, accounting"
                           : "verbose normal");
            } else if (cmd == "/plan") {
                if (!todos.has_items()) {
                    scene.note("no plan yet: the model writes one with the todo tool");
                } else {
                    std::string out;
                    for (const auto& it : todos.items) {
                        const char* g = it.status == "in_progress" ? "▶ " : it.status == "completed" ? "✓ "
                                        : it.status == "cancelled" ? "✗ " : "○ ";
                        out += (out.empty() ? "" : "\n") + std::string(g) + it.content;
                    }
                    scene.note(out);
                }
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
                } else if (cmd_busy.load()) {
                    scene.note("still running: " + cmd_label);
                } else {
                    engine.reset();
                    scene.clear();
                    todos.items.clear();
                    publish_plan();   // a new session starts with no plan
                    scene.note("new session " + engine.session().id() + " · the maze still has the last one");
                }
            } else if (cmd == "/context") {
                auto report = [&scene, &engine] {
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
                };
                if (running.load()) report();   // mid-turn: the published numbers, nothing is drained
                else run_cmd("context", [&engine, report] {
                    engine.sync();
                    report();
                });
            } else if (cmd == "/resume") {
                const std::string arg = trim_ws(p.substr(cmd.size()));
                if (running.load()) {
                    scene.note("/resume applies between turns");
                } else {
                    run_cmd("resume", [&, arg] {
                        if (arg.empty()) {
                            std::string list;
                            for (const auto& r : engine.recent_sessions("dd-", 8))
                                list += "\n  " + r.id + " · " + std::to_string(r.turns) + " turns · " + r.last;
                            scene.note(list.empty() ? std::string("no dd sessions in the maze yet")
                                                    : "sessions in the maze (/resume <id>, any auto:turn session works):" + list);
                            return;
                        }
                        std::string why, sid = arg;
                        if (arg == "last")   // the newest session that is not this one
                            for (const auto& r : engine.recent_sessions("dd-", 3))
                                if (r.id != engine.session().id()) {
                                    sid = r.id;
                                    break;
                                }
                        scene.clear();
                        if (!engine.resume(sid, &why)) scene.error("resume " + sid + ": " + why);
                    });
                }
            } else if (cmd == "/rebase") {
                if (running.load()) scene.note("/rebase applies between turns");
                else run_cmd("rebase", [&engine] { engine.rebase_now(); });
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
            } else if (serv) {
                const std::string rest = trim_ws(p.substr(cmd.size()));
                std::string tool, args;
                if (!rest.empty()) {
                    const size_t sp = rest.find(' ');
                    const std::string head = sp == std::string::npos ? rest : rest.substr(0, sp);
                    args = sp == std::string::npos ? "" : trim_ws(rest.substr(sp + 1));
                    for (const auto& t : serv->tools)
                        if (t == head) { tool = t; break; }
                    if (tool.empty()) {           // unique prefix of exactly one registered tool wins
                        int n = 0;
                        for (const auto& t : serv->tools)
                            if (t.rfind(head, 0) == 0) { tool = t; ++n; }
                        if (n != 1) { tool.clear(); args.clear(); }  // not a verb: treat all of rest as a query
                    }
                }
                if (!tool.empty()) invoke(*serv, tool, args);
                else if (!rest.empty()) {
                    // "/pulse bitcoin" — the server's own default verb, straight through: no model, no search detour
                    std::string def = "search";
                    bool has = false;
                    for (const auto& t : serv->tools)
                        if (t == def || t == serv->cmd.substr(1) + "_search") { has = true; break; }
                    if (!has) {
                        for (const auto& t : serv->tools)
                            if (t.rfind("research", 0) == 0 || t.rfind(serv->cmd.substr(1) + "_research", 0) == 0) { def = t; break; }
                    }
                    bool found = false;
                    for (const auto& t : serv->tools)
                        if (t == def || t == serv->cmd.substr(1) + "_" + def) { found = true; break; }
                    if (!found) { scene.note(serv->cmd + " has no default verb · " + serv->cmd + " lists its tools"); return; }
                    invoke(*serv, def, rest);
                } else invoke(*serv, "", "");
            } else if (cmd == "/mcp") {   // what is registered: every server that answered at boot
                std::string l = "live MCP servers — each drives itself with /<name> [tool] [json]:";
                if (cmd_servs.empty()) l += " none";
                for (const auto& sv : cmd_servs) {
                    l += " " + sv.cmd;
                    for (const auto& m : mcps)
                        if (m->url == sv.url) { l += " (" + m->url + ", " + std::to_string(sv.wire.size()) + " tools)"; break; }
                }
                scene.note(l);
            } else {
                scene.note("unknown command " + cmd + " · /help — /mcp lists the live servers");
            }
            return;
        }
        if (running.load() || cmd_busy.load()) {
            size_t n;
            {
                std::lock_guard<std::mutex> lk(qmu);
                queued.push_back(p);
                n = queued.size();
            }
            // A turn pulls at its next round boundary, so this redirect lands mid-turn; a command
            // cannot, so it waits. If the turn ends before a boundary, the idle loop sends it as its
            // own turn — never silently.
            scene.note("queued (" + std::to_string(n) + ") — " +
                       (running.load() ? "enters this turn at its next round boundary, or becomes a turn if the turn ends first"
                                       : "sends when the " + cmd_label + " ends"));
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
        case K::Esc:
            if (running.load()) interrupt();
            else if (cmd_busy.load()) scene.note("a command is running (" + cmd_label + "): it cannot be interrupted");
            else ed.clear();
            break;
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

    using Clock = std::chrono::steady_clock;
    constexpr auto kEscWait = std::chrono::milliseconds(25);      // silence after a lone Esc: it was the key
    constexpr auto kPartialWait = std::chrono::milliseconds(250); // a sequence that never finished
    std::optional<Clock::time_point> held_since;                  // when the reader began holding an Esc
    char rbuf[4096];
    while (!quit) {
        if (g_sig.load()) {   // INT/TERM/HUP: leave the way Ctrl-C does
            quit = true;
            break;
        }
        pollfd pfd{STDIN_FILENO, POLLIN, 0};
        // 20 fps while the spinner turns, ~8 fps idle (frames with no change write nothing); 25 ms while an Esc is held
        const int wait_ms = keys.esc_pending() ? static_cast<int>(kEscWait.count()) : running.load() ? 50 : 120;
        const int pr = ::poll(&pfd, 1, wait_ms);
        if (pr > 0) {
            const ssize_t n = ::read(STDIN_FILENO, rbuf, sizeof rbuf);
            if (n > 0) {
                for (const Key& k : keys.feed(rbuf, static_cast<size_t>(n))) handle(k);
            } else if (pfd.revents & (POLLHUP | POLLERR)) {
                quit = true;   // the terminal went away
            }
        }
        if (keys.esc_pending() || keys.partial_pending()) {
            const auto now = Clock::now();
            if (!held_since) held_since = now;
            if (keys.esc_pending() && (pr == 0 || now - *held_since >= kEscWait)) {
                for (const Key& k : keys.flush_esc()) handle(k);
                held_since.reset();
            } else if (keys.partial_pending() && now - *held_since >= kPartialWait) {
                keys.drop_partial();
                held_since.reset();
            }
        } else {
            held_since.reset();
        }
        if (!running.load() && worker.joinable()) worker.join();
        if (!cmd_busy.load() && cmd_thread.joinable()) cmd_thread.join();
        std::string q;
        if (!running.load() && !cmd_busy.load() && !quit) {
            std::lock_guard<std::mutex> lk(qmu);
            if (!queued.empty()) {
                q = std::move(queued.front());
                queued.pop_front();
                if (!queued.empty()) scene.note("still queued (" + std::to_string(queued.size()) + ") — the rest follow this turn");
            }
        }
        if (!q.empty()) start_turn(q);
        term::size(cols, rows);
        std::string hint;
        if (const std::string typed = dd::ui::to_utf8(ed.buf); is_command(typed) && typed.find(' ') == std::string::npos) {
            for (const auto& m : command_matches(typed)) hint += (hint.empty() ? "" : "  ") + m;
            if (hint.empty()) hint = "no such command";
        } else if (typed.rfind("/effort ", 0) == 0 || typed.rfind("/reasoning ", 0) == 0 || typed.rfind("/verbose ", 0) == 0) {
            for (const auto& m : arg_matches(typed, efforts)) hint += (hint.empty() ? "" : "  ") + m;
            if (hint.empty()) hint = efforts.empty() ? "this model has no reasoning switch" : "not offered by this model";
        }
        scene.set_input(ed.buf, ed.cur, hint);
        term::put(scene.present(cols, rows, truecolor));
    }

    // Quit: the turn stops at its next step (and a running command's process group with it), a command gets the same
    // two seconds; whatever still holds on is killed, then the process leaves.
    auto busy = [&] { return running.load() || cmd_busy.load(); };
    agent.interrupt_requested = true;
    for (int i = 0; i < 40 && busy(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (busy()) {
        dd::tools::shell_kill_all();
        for (int i = 0; i < 10 && busy(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    term::leave();
    term::show_stderr(saved_err);
    {   // what was typed and never reached the model: say it, do not drop it quietly
        std::lock_guard<std::mutex> lk(qmu);
        if (!queued.empty())
            std::cout << std::to_string(queued.size()) + " prompt(s) typed were never sent\n";
    }
    {
        const auto& th = scene.theme();
        dd::ui::Canvas c(std::max(1, dd::ui::text_width(th.goodbye) + 1), 1);
        c.text(0, 0, th.goodbye, dd::ui::Style{th.accent, dd::ui::kNone, dd::ui::kItalic});
        const std::string s = c.to_ansi(color, truecolor);
        std::fwrite(s.data(), 1, s.size(), stdout);
        std::fflush(stdout);
    }
    const int exit_sig = g_sig.load();
    dd::tools::shell_kill_all();
    if (busy()) std::_Exit(exit_sig ? 128 + exit_sig : 0);   // a call that would not stop still holds a thread
    if (worker.joinable()) worker.join();
    if (cmd_thread.joinable()) cmd_thread.join();
    return exit_sig ? 128 + exit_sig : 0;
}
