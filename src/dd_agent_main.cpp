// src/dd_agent_main.cpp — the running C++ agent binary (build/dd-agent).
//
// Drives the faithfully-ported run loop (dd/agent/agent.cpp) against a real
// OpenAI-compatible chat-completions endpoint (config model.base_url) via
// libcurl. Ships a working filesystem tool set (read_file / write_file /
// list_dir) with real I/O so the agent actually executes a turn.
//
// usage:
//   build/dd-agent                                 # config.yaml beside the binary, msg on stdin
//   build/dd-agent path/to/config.yaml -- "msg"    # explicit config, msg on argv
// env overrides: DD_BASE_URL, DD_MODEL, DD_API_KEY, DD_MAX_TURNS

#include "dd/agent/agent.hpp"
#include "dd/core/config.hpp"
#include "dd/core/home.hpp"
#include "dd/core/json.hpp"
#include "dd/core/message.hpp"
#include "dd/tools/todo_tool.hpp"
#include "dd/tools/fs_tools.hpp"
#include "dd/tools/registry.hpp"
#include "dd/tools/tool.hpp"

#include <curl/curl.h>
#include <filesystem>
#include <cstdlib>
namespace ddcore = ::dd::core;
using namespace dd::core;
using dd::tools::ToolEntry;
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <memory>

namespace ddcore = ::dd::core;

using JsonSP = ddcore::JsonSP;


// libcurl 8.16+ write-callback: (buffer, capacity, actual_bytes, outstream).
static std::size_t curl_sink(char* buffer, std::size_t size, std::size_t nitems, void* ud) {
    (void)size;
    auto* s = static_cast<std::string*>(ud);
    s->append(buffer, nitems);
    return nitems;
}

// ---- concrete provider transport: OpenAI chat/completions over libcurl ----
struct HttpLLMClient : dd::agent::LLMClient {
    std::string base_url;   // e.g. http://127.0.0.1:8080/v1
    std::string model;
    std::string api_key;

    dd::core::Message chat(const std::vector<dd::core::Message>& messages,
                           const dd::core::Json& tools) override {
        // ---- request body: {model, messages:[...], tools:[...]} ----
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
        struct CurlGuard { CURL* c; ~CurlGuard(){ curl_easy_cleanup(c);} } guard{c};

        struct curl_slist* hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
        if (!api_key.empty()) {
            std::string auth = "Authorization: Bearer " + api_key;
            hdrs = curl_slist_append(hdrs, auth.c_str());
        }
        struct SListGuard { struct curl_slist* h; ~SListGuard(){ curl_slist_free_all(h);} } hguard{hdrs};

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

        // ---- parse choices[0] ----
        Json resp = json_parse(out);
        const Json* choices = json_get(resp, "choices");
        dd::core::Message assistant;
        if (choices && choices->type == Json::Arr && !choices->arr.empty()) {
            const Json& ch0 = *choices->arr[0];
            if (const Json* msg = json_get(ch0, "message"))
                assistant = ddcore::message_from_json(*msg);
            if (assistant.finish_reason.empty()) {
                if (const Json* fr = json_get(ch0, "finish_reason"))
                    assistant.finish_reason = fr->type == Json::Str ? fr->str : "";
            }
        }
        if (assistant.role.empty()) assistant.role = "assistant";
        return assistant;
    }
};

// ---- working filesystem tool handlers (real I/O) ----
static std::string h_read_file(const Json& args) { return dd::tools::fs_read(args); }
static std::string h_write_file(const Json& args) { return dd::tools::fs_write(args); }
static std::string h_list_dir(const Json& args) { return dd::tools::fs_list(args); }
static ddcore::Json fs_schema(const std::vector<std::string>& props, const std::string& desc) { return dd::tools::fs_tool_schema(props, desc); }
static void register_fs_tools(dd::tools::ToolRegistry& reg) {
    { ToolEntry e; e.name = "read_file"; e.schema = fs_schema({"path"}, "Read a file's UTF-8 text content."); e.handler = h_read_file; reg.register_tool(e); }
    { ToolEntry e; e.name = "write_file"; e.schema = fs_schema({"path","content"}, "Write text content to a file."); e.handler = h_write_file; reg.register_tool(e); }
    { ToolEntry e; e.name = "list_dir"; e.schema = fs_schema({"path"}, "List entries of a directory."); e.handler = h_list_dir; reg.register_tool(e); }
}

// ---- entry ----
int main(int argc, char** argv) {
    std::string home_flag, config_flag;
    std::vector<std::string> msg_words;
    bool after_dd = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (after_dd) msg_words.push_back(a);
        else if (a == "--") after_dd = true;
        else if (a == "--home" && i + 1 < argc) home_flag = argv[++i];
        else if (a == "--config" && i + 1 < argc) config_flag = argv[++i];
        else config_flag = a;
    }
    // config.yaml beside the binary (see dd_tui --init), not the Python Daedalus's ~/.daedalus
    std::string why;
    const ddcore::Home home = ddcore::resolve_home(home_flag, config_flag, &why);
    if (home.root.empty()) { std::cerr << why << "\n"; return 2; }
    ddcore::set_daedalus_home_override(home.root.string());
    const std::string config_path = config_flag.empty() ? home.config().string() : config_flag;

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
    if (const char* v = std::getenv("DD_MAX_TURNS")) {
        try { mt = std::stoll(v); } catch (...) { mt = 0; }   // a bad value falls back to the default
    }
    else { long long t = cfg.get_int("agent.max_turns"); mt = t; }
    int max_iterations = (mt > 0) ? (int)mt : 90;

    if (base_url.empty()) { std::cerr << "no model.base_url in config and DD_BASE_URL unset\n"; return 2; }

    dd::tools::ToolRegistry reg;
    register_fs_tools(reg);

    HttpLLMClient llm;
    llm.base_url = base_url;
    llm.model = model;
    llm.api_key = api_key;

    dd::agent::Agent agent;
    agent.model = model;
    agent.max_iterations = max_iterations;
    agent.quiet_mode = true;
    agent.registry = &reg;
    agent.llm = &llm;

    // the same ledger the TUI keeps: the plan rides the ephemeral prompt, not the tape
    dd::tools::TodoStore todos;
    {
        auto j = [](Json v) { return std::make_shared<Json>(std::move(v)); };
        ToolEntry e;
        e.name = "todo";
        Json item = json_obj({{"id", j(json_obj({{"type", j(json_str("string"))}}))},
                              {"content", j(json_obj({{"type", j(json_str("string"))}}))},
                              {"status", j(json_obj({{"type", j(json_str("string"))},
                                                     {"enum", j(json_arr({j(json_str("pending")), j(json_str("in_progress")), j(json_str("completed")), j(json_str("cancelled"))}))}}))}});
        Json props = json_obj({{"todos", j(json_obj({{"type", j(json_str("array"))}, {"items", j(std::move(item))}}))},
                               {"merge", j(json_obj({{"type", j(json_str("boolean"))}}))}});
        e.schema = json_obj({{"description", j(json_str("Work ledger for a multi-step task. Write the plan before acting, "
                                                        "update statuses as you go; omit todos to read the list back. "
                                                        "merge=true updates only the items named."))},
                             {"parameters", j(json_obj({{"type", j(json_str("object"))}, {"properties", j(std::move(props))}}))}});
        e.handler = [&todos, &agent](const Json& a) {
            const Json* t = json_get(a, "todos");
            const Json* m = json_get(a, "merge");
            const bool merge = m && m->type == Json::Bool && m->b;
            const std::string r = dd::tools::todo_tool(t ? *t : Json{}, merge, todos);
            const auto inj = todos.format_for_injection();
            agent.ephemeral_system_prompt = inj ? *inj : "";
            return r;
        };
        reg.register_tool(e);
    }

    std::string user_message;
    if (!msg_words.empty()) {
        for (size_t i = 0; i < msg_words.size(); ++i) { if (i) user_message += " "; user_message += msg_words[i]; }
    } else {
        std::string line;
        while (std::getline(std::cin, line)) { if (!user_message.empty()) user_message += "\n"; user_message += line; }
    }

    if (user_message.empty()) { std::cerr << "empty user message\n"; return 1; }

    std::cout << "[base=" << base_url << " model=" << model << " turns=" << max_iterations << "]\n";
    std::string final = agent.run(user_message);
    std::cout << "=== final (" << agent.api_call_count << " LLM calls) ===\n" << final << "\n";
    return 0;
}
