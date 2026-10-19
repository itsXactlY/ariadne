// src/dd_agent_main.cpp — the running C++ agent binary (build/dd-agent).
//
// Drives the faithfully-ported run loop (dd/agent/agent.cpp) against a real
// OpenAI-compatible chat-completions endpoint (config model.base_url) via
// libcurl. Ships a working filesystem tool set (read_file / write_file /
// list_dir) with real I/O so the agent actually executes a turn.
//
// usage:
//   build/dd-agent                                 # config from ~/.daedalus/config.yaml, msg on stdin
//   build/dd-agent path/to/config.yaml -- "msg"    # explicit config, msg on argv
// env overrides: DD_BASE_URL, DD_MODEL, DD_API_KEY, DD_MAX_TURNS

#include "dd/agent/agent.hpp"
#include "dd/core/config.hpp"
#include "dd/core/json.hpp"
#include "dd/core/message.hpp"
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

// ---- tiny builders: turn a Json value into a JsonSP (for init-lists) ----
static JsonSP jsp(Json v) { return std::make_shared<Json>(std::move(v)); }

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
static std::string jerr(const std::string& m) {
    return json_dump(json_obj({{"error", jsp(json_str(m))}}));
}
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

// Register the fs tools with faithful OpenAI-style schemas.
static ddcore::Json fs_schema(const std::vector<std::string>& props, const std::string& desc) {
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
static void register_fs_tools(dd::tools::ToolRegistry& reg) {
    { ToolEntry e; e.name = "read_file"; e.schema = fs_schema({"path"}, "Read a file's UTF-8 text content."); e.handler = h_read_file; reg.register_tool(e); }
    { ToolEntry e; e.name = "write_file"; e.schema = fs_schema({"path","content"}, "Write text content to a file."); e.handler = h_write_file; reg.register_tool(e); }
    { ToolEntry e; e.name = "list_dir"; e.schema = fs_schema({"path"}, "List entries of a directory."); e.handler = h_list_dir; reg.register_tool(e); }
}

// ---- entry ----
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
