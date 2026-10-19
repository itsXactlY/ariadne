// src/dd_tui.cpp — the full agentic harness kit: a tiny ANSI terminal REPL
// that drives the faithfully-ported run loop (dd/agent/agent.cpp) against a
// real OpenAI-compatible endpoint, with REAL tools wired in:
//
//   * filesystem tools (read_file / write_file / list_dir) — real I/O
//   * run_terminal      — allowed except a denylist (danger_reason)
//   * every enabled mazemaker MCP tool — discovered live from the MCP server's
//     tools/list and dispatched through the Streamable-HTTP MCP client
//
// The MCP server's own "instructions" (the MAZEMAKER MEMORY PROTOCOL) are
// injected as the agent system prompt, so recall-first / persist-after-turn
// behaviour is real, not simulated. Continuity across REPL turns rides on the
// external memory graph (recall), not an in-process transcript — that is the
// protocol's own design.
//
// usage:
//   build/dd_tui                                 # config from ~/.daedalus, interactive REPL
//   build/dd_tui path/to/config.yaml -- "msg"    # explicit config, single-shot msg
// env overrides: DD_BASE_URL, DD_MODEL, DD_API_KEY, DD_MAX_TURNS

#include "dd/agent/agent.hpp"
#include "dd/core/config.hpp"
#include "dd/core/json.hpp"
#include "dd/core/message.hpp"
#include "dd/tools/mcp.hpp"
#include "dd/tools/registry.hpp"
#include "dd/tools/tool.hpp"

#include <curl/curl.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace ddcore = ::dd::core;
using namespace dd::core;
using dd::tools::ToolEntry;
using dd::tools::MCPClient;

using JsonSP = ddcore::JsonSP;

// ---- tiny builders: Json value -> JsonSP (for init-lists) ----
static JsonSP jsp(Json v) { return std::make_shared<Json>(std::move(v)); }

// libcurl 8.16+ write callback: (buffer, capacity, actual_bytes, outstream).
static std::size_t curl_sink(char* buffer, std::size_t size, std::size_t nitems, void* ud) {
    (void)size;
    auto* s = static_cast<std::string*>(ud);
    s->append(buffer, nitems);
    return nitems;
}

static std::string jerr(const std::string& m) {
    return json_dump(json_obj({{"error", jsp(json_str(m))}}));
}

// ---- concrete provider transport: OpenAI chat/completions over libcurl ----
struct HttpLLMClient : dd::agent::LLMClient {
    std::string base_url, model, api_key;

    dd::core::Message chat(const std::vector<dd::core::Message>& messages,
                           const dd::core::Json& tools) override {
        Json body = json_obj();
        obj_set(body, "model", json_str(model));
        Json msgs = json_arr();
        for (const auto& m : messages) arr_push(msgs, ddcore::message_to_json(m));
        obj_set(body, "messages", msgs);
        if (tools.type == Json::Arr && !tools.arr.empty()) obj_set(body, "tools", tools);
        obj_set(body, "stream", json_bool(false));

        std::string url = base_url;
        if (!url.empty() && url.back() != '/') url += "/";
        url += "chat/completions";

        std::string payload = json_dump(body);
        std::string out;

        CURL* c = curl_easy_init();
        if (!c) throw std::runtime_error("curl: out of memory for handle");
        struct CurlGuard { CURL* c; ~CurlGuard() { curl_easy_cleanup(c); } } guard{c};

        struct curl_slist* hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
        if (!api_key.empty()) {
            std::string auth = "Authorization: Bearer " + api_key;
            hdrs = curl_slist_append(hdrs, auth.c_str());
        }
        struct SListGuard { struct curl_slist* h; ~SListGuard() { curl_slist_free_all(h); } } hguard{hdrs};

        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (curl_off_t)payload.size());
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_sink);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);

        CURLcode rc = curl_easy_perform(c);
        if (rc != CURLE_OK)
            throw std::runtime_error(std::string("curl: ") + curl_easy_strerror(rc));

        Json resp = json_parse(out);
        dd::core::Message assistant;
        if (const Json* choices = json_get(resp, "choices");
            choices && choices->type == Json::Arr && !choices->arr.empty()) {
            const Json& ch0 = *choices->arr[0];
            if (const Json* msg = json_get(ch0, "message"))
                assistant = ddcore::message_from_json(*msg);
            if (assistant.finish_reason.empty())
                if (const Json* fr = json_get(ch0, "finish_reason"))
                    assistant.finish_reason = fr->type == Json::Str ? fr->str : "";
        }
        if (assistant.role.empty()) assistant.role = "assistant";
        return assistant;
    }
};

// ---- filesystem tool handlers (real I/O) ----
static std::string h_read_file(const Json& args) {
    const Json* p = json_get(args, "path");
    if (!p || p->type != Json::Str) return jerr("path required");
    std::ifstream f(p->str, std::ios::binary);
    if (!f) return jerr("cannot open " + p->str);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return json_dump(json_obj({{"path", jsp(json_str(p->str))},
                               {"content", jsp(json_str(s))}}));
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

    // Split into simple-command segments on shell separators, then inspect each.
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

    std::string full = cmd + " 2>&1";
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

// ---- ANSI colour helpers for the streaming printer ----
static std::string clip(const std::string& s, std::size_t n) {
    return s.size() <= n ? s : s.substr(0, n) + "...";
}

int main(int argc, char** argv) {
    const char* h_env = std::getenv("HOME");
    std::string home = h_env ? h_env : std::string("/home/alca");
    std::string config_path = home + "/.daedalus/config.yaml";
    std::vector<std::string> msg_words;
    bool after_dd = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--") { after_dd = true; continue; }
        if (!after_dd) config_path = a;
        else msg_words.push_back(a);
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

    // ---- build the registry: fs tools + guarded terminal ----
    dd::tools::ToolRegistry reg;
    { ToolEntry e; e.name = "read_file"; e.schema = fs_schema({"path"}, "Read a file's UTF-8 text content."); e.handler = h_read_file; reg.register_tool(e); }
    { ToolEntry e; e.name = "write_file"; e.schema = fs_schema({"path", "content"}, "Write text content to a file."); e.handler = h_write_file; reg.register_tool(e); }
    { ToolEntry e; e.name = "list_dir"; e.schema = fs_schema({"path"}, "List entries of a directory."); e.handler = h_list_dir; reg.register_tool(e); }
    {
        ToolEntry e;
        e.name = "run_terminal";
        {
            Json rt = json_arr();
            arr_push(rt, json_str("cmd"));
            Json cmdp = json_obj({{"type", jsp(json_str("string"))},
                                  {"description", jsp(json_str("command to run"))}});
            Json props = json_obj({{"cmd", jsp(std::move(cmdp))}});
            Json params = json_obj({{"type", jsp(json_str("object"))},
                                    {"properties", jsp(std::move(props))},
                                    {"required", jsp(std::move(rt))}});
            e.schema = json_obj({{"description", jsp(json_str("Run a shell command. Dangerous commands (sudo, su, rm -rf /, mkfs, dd, shutdown, force-push, ...) are refused. Returns exit code + stdout."))},
                                 {"parameters", jsp(std::move(params))}});
        }
        e.handler = [&cfg](const Json& a) { return run_terminal(a, cfg); };
        reg.register_tool(e);
    }

    // ---- discover + register the live MCP servers from config ----
    // Takes precedence over the MCP protocol below: live machine state is measured, not recalled.
    std::string system_prompt =
        "Tool use: for LIVE state (VRAM/GPU, RAM, disk, processes, files, the running model and its context "
        "size) call run_terminal or read_file FIRST, e.g. `nvidia-smi --query-gpu=memory.used,memory.total "
        "--format=csv`, `free -h`, `curl -s http://127.0.0.1:8080/props`. Memory recall is only for past "
        "decisions and history, never for current measurements.";
    if (std::string cp = cfg.get_string("agent.system_prompt"); !cp.empty()) system_prompt += "\n\n" + cp;
    std::vector<std::unique_ptr<MCPClient>> mcps;  // stable addresses
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
                std::string instr = mcp->instructions();
                if (!instr.empty()) {
                    if (!system_prompt.empty()) system_prompt += "\n\n";
                    system_prompt += instr;
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
                        e.handler = [m = mcp.get(), tname](const Json& a) {
                            return m->call(tname, a);
                        };
                        reg.register_tool(e);
                        ++registered;
                    }
                }
                std::cerr << "[mcp] server '" << srvname << "' at " << mcp->url << ": "
                          << registered << " tools registered\n";
                mcps.push_back(std::move(mcp));
            } catch (const std::exception& ex) {
                std::cerr << "[mcp] server '" << srvname << "' unavailable: " << ex.what() << "\n";
            }
        }
    }

    // ---- assemble the agent + streaming printer ----
    HttpLLMClient llm;
    llm.base_url = base_url;
    llm.model = model;
    llm.api_key = api_key;

    dd::agent::Agent agent;
    agent.model = model;
    agent.max_iterations = max_iterations;
    agent.quiet_mode = true;
    agent.system_prompt = system_prompt;
    agent.registry = &reg;
    agent.llm = &llm;

    std::map<std::string, std::string> names;  // tool_call_id -> name (per turn)
    bool printed_final = false;
    agent.step_hook = [&](const dd::core::Message& m) {
        if (m.has_tool_calls()) {
            if (!m.content.empty()) std::cout << "\033[2;37m" << clip(m.content, 240) << "\033[0m\n";
            for (const auto& tc : m.tool_calls) {
                names[tc.id] = tc.name;
                std::cout << "  \033[36m\u25b8 " << tc.name << "\033[0m "
                          << clip(tc.arguments, 200) << "\n" << std::flush;
            }
        } else if (m.role == "tool") {
            std::string nm = names.count(m.tool_call_id) ? names[m.tool_call_id] : "?";
            std::cout << "  \033[32m\u25c2 " << nm << "\033[0m " << clip(m.content, 600)
                      << "\n" << std::flush;
        } else if (m.role == "assistant") {
            if (!m.content.empty()) {
                std::cout << "\033[34massistant\033[0m " << m.content << "\n" << std::flush;
                printed_final = true;
            }
        }
    };

    const std::string c_reset = "\033[0m";
    const std::string c_dim = "\033[2m";

    // one-shot mode: message after `--` on argv
    if (!msg_words.empty()) {
        std::string user_message;
        for (size_t i = 0; i < msg_words.size(); ++i) { if (i) user_message += " "; user_message += msg_words[i]; }
        std::cout << c_dim << "run(" << agent.api_call_count << ")" << c_reset << "\n";
        std::string fin = agent.run(user_message);
        if (!printed_final && !fin.empty()) std::cout << "\033[34massistant\033[0m " << fin << "\n";
        return 0;
    }

    // interactive REPL
    std::cout << "\033[1mdd_tui\033[0m  model=" << model << "  base=" << base_url
              << "  tools=" << reg.get_all_tool_names().size()
              << "  turns=" << max_iterations << "\n"
              << c_dim << "commands: /tools  /turns N  /quit   (anything else = prompt to the agent)\n"
              << c_reset << "\n";

    std::string line;
    for (;;) {
        std::cout << "\033[1myou>\033[0m " << std::flush;
        if (!std::getline(std::cin, line)) break;
        // trim
        std::size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        std::size_t e = line.find_last_not_of(" \t");
        line = line.substr(b, e - b + 1);

        if (line == "/quit" || line == "/exit" || line == ":q") break;
        if (line == "/tools") {
            for (const auto& n : reg.get_all_tool_names()) std::cout << "  " << n << "\n";
            continue;
        }
        if (line == "/help") {
            std::cout << c_dim << "/tools list tools | /turns N set max iterations | /quit exit" << c_reset << "\n";
            continue;
        }
        if (line.rfind("/turns ", 0) == 0) {
            try { agent.max_iterations = std::stoi(line.substr(7)); } catch (...) {}
            std::cout << c_dim << "max_iterations=" << agent.max_iterations << c_reset << "\n";
            continue;
        }

        names.clear();
        printed_final = false;
        std::string fin = agent.run(line);
        if (!printed_final && !fin.empty()) std::cout << "\033[34massistant\033[0m " << fin << "\n";
    }
    return 0;
}
