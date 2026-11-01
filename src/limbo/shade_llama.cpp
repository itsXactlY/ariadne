// src/limbo/shade_llama.cpp — impl of dd/limbo/shade_llama.hpp.
#include "dd/limbo/shade_llama.hpp"

#include "dd/core/park.hpp"
#include "dd/limbo/spawn.hpp"
#include "dd/net/http.hpp"

#include <cstring>
#include <algorithm>
#include <sys/wait.h>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace dd {
namespace limbo {

namespace {

std::string join_url(std::string base, const std::string& path) {
    if (!base.empty() && base.back() == '/') base.pop_back();
    return base + path;
}

std::chrono::steady_clock::time_point now() { return std::chrono::steady_clock::now(); }

long elapsed_ms(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(now() - since).count();
}

// Pull one complete SSE event out of a growing buffer. The byte boundary is wherever curl
// happened to stop, so an event can arrive split; only a finished one is parsed.
bool take_event(std::string& buf, std::string& event) {
    const size_t nl = buf.find('\n');
    if (nl == std::string::npos) return false;
    std::string line = buf.substr(0, nl);
    buf.erase(0, nl + 1);
    if (line.empty() || line == "\r") return false;
    if (!line.starts_with("data:")) return false;      // a comment, an event name, keep-alive
    std::string payload = line.substr(5);
    if (!payload.empty() && payload.front() == ' ') payload.erase(0, 1);
    if (!payload.empty() && payload.back() == '\r') payload.pop_back();
    event = std::move(payload);
    return true;
}

// A tool call under construction. llama.cpp streams a call as fragments keyed by index:
// the id and name come once, the arguments arrive in pieces.
struct CallFragment {
    std::string id, name, arguments;
};

}  // namespace

core::Json llama_body(const TurnRequest& req, const std::string& model) {
    core::Json b = core::json_obj();
    core::obj_set(b, "model", core::json_str(model));
    core::Json msgs = core::json_arr();
    for (const auto& m : req.messages) {
        core::Json w = core::message_to_json(m);
        // Bookkeeping stays home; reasoning goes out under the name the server reads.
        for (auto it = w.obj.begin(); it != w.obj.end();) {
            if (it->first == "finish_reason") { it = w.obj.erase(it); continue; }
            if (it->first == "reasoning") it->first = "reasoning_content";
            ++it;
        }
        core::arr_push(msgs, w);
    }
    core::obj_set(b, "messages", msgs);
    core::obj_set(b, "stream", core::json_bool(req.stream));
    if (req.sampling.max_tokens >= 0) core::obj_set(b, "max_tokens", core::json_num(req.sampling.max_tokens));
    if (req.sampling.temperature > 0.0) core::obj_set(b, "temperature", core::json_num(req.sampling.temperature));
    if (req.sampling.top_k > 0) core::obj_set(b, "top_k", core::json_num(req.sampling.top_k));
    if (req.sampling.top_p < 1.0) core::obj_set(b, "top_p", core::json_num(req.sampling.top_p));
    if (req.sampling.min_p > 0.0) core::obj_set(b, "min_p", core::json_num(req.sampling.min_p));
    if (req.sampling.repetition_penalty != 1.0) core::obj_set(b, "repeat_penalty", core::json_num(req.sampling.repetition_penalty));
    if (req.sampling.seed > 0) core::obj_set(b, "seed", core::json_num(req.sampling.seed));
    if (!req.sampling.effort.empty()) core::obj_set(b, "reasoning_effort", core::json_str(req.sampling.effort));
    if (req.tools.type == core::Json::Arr && !req.tools.arr.empty()) core::obj_set(b, "tools", req.tools);
    return b;
}

ShadeLlama::ShadeLlama(ShadeSpec spec) : spec_(std::move(spec)) {
    if (!spec_.base_url.empty()) base_url_ = spec_.base_url;
    else if (spec_.port > 0) base_url_ = "http://127.0.0.1:" + std::to_string(spec_.port);
    else base_url_ = "http://127.0.0.1:8080";
    model_ = spec_.model_id;
}

ShadeLlama::~ShadeLlama() { stop(); }

bool ShadeLlama::probe(std::string& error) {
    const net::Response r = net::get(join_url(base_url_, "/props"), 4000);
    if (!r.ok()) {
        error = r.error.empty() ? "the llama dweller answered " + std::to_string(r.status) + " to /props"
                                : "the llama dweller: " + r.error;
        alive_.store(false, std::memory_order_release);
        return false;
    }
    try {
        core::Json p = core::json_parse(r.body);
        if (p.type == core::Json::Obj) {
            const core::Json* n = core::json_get(p, "n_ctx");
            if (n && n->type == core::Json::Num) n_ctx_.store(static_cast<int>(n->num), std::memory_order_release);
            const core::Json* s = core::json_get(p, "slots");
            if (!s) s = core::json_get(p, "n_parallel");
            if (s && s->type == core::Json::Num) slots_.store(static_cast<int>(s->num), std::memory_order_release);
            const core::Json* alias = core::json_get(p, "model_alias");
            if (!alias) alias = core::json_get(p, "model");
            if (alias && alias->type == core::Json::Str && !alias->str.empty()) model_ = alias->str;
        }
    } catch (const std::exception& e) {
        error = std::string("the llama dweller's /props is not JSON: ") + e.what();
        return false;
    }
    if (spec_.n_ctx > 0 && n_ctx_.load(std::memory_order_acquire) != spec_.n_ctx) {
        error = "the llama dweller reports a " + std::to_string(n_ctx_.load(std::memory_order_acquire)) +
                " token window, this level asked for " + std::to_string(spec_.n_ctx);
        return false;
    }
    alive_.store(true, std::memory_order_release);
    return true;
}

bool ShadeLlama::start(std::string& error) {
    abort_.store(false, std::memory_order_release);
    if (spec_.exe.empty()) {
        // Adopted, not owned: a server someone else started is probed and used, never ended.
        return probe(error);
    }
    reap_leftover(spec_.exe, ::getpid());
    std::vector<std::string> argv;
    argv.push_back(spec_.exe);
    for (const auto& a : spec_.args) argv.push_back(a);
    if (spec_.port > 0) {
        argv.push_back("--host");
        argv.push_back("127.0.0.1");
        argv.push_back("--port");
        argv.push_back(std::to_string(spec_.port));
    }
    std::string sink_note;
    SpawnStdio stdio;
    stdio.log_fd = open_log_sink(spec_.log, sink_note);
    if (stdio.log_fd < 0) {
        error = "the llama dweller's stderr has nowhere to go: " + std::string(strerror(errno));
        return false;
    }
    pid_ = spawn_process(spec_.exe, argv, spec_.cwd, spec_.env, stdio, error);
    if (pid_ < 0) {
        if (!sink_note.empty()) error += " (" + sink_note + ")";
        return false;
    }
    // A server is ready when /props answers, not when the process exists: a load of a few GB
    // is a window in which every turn would be refused.
    const long ceiling_ms = spec_.ready_timeout_s > 0 ? static_cast<long>(spec_.ready_timeout_s) * 1000 : 120000;
    const auto began = now();
    std::atomic<uint32_t> idle{0};
    while (true) {
        if (probe(error)) return true;
        if (elapsed_ms(began) >= ceiling_ms) {
            error = spec_.exe + " never answered /props within " + std::to_string(ceiling_ms / 1000) + " s";
            std::string end_error;
            end_process(pid_, 5000, end_error);
            pid_ = -1;
            return false;
        }
        int status = 0;
        if (::waitpid(pid_, &status, WNOHANG) == pid_) {
            error = spec_.exe + " exited while loading (code " + std::to_string(status) + ")";
            pid_ = -1;
            alive_.store(false, std::memory_order_release);
            return false;
        }
        core::park(idle, 0, std::chrono::milliseconds(250));   // probed on a park, never polled tight
    }
}

Props ShadeLlama::props() const {
    Props p;
    p.kind = ShadeKind::Llama;
    p.model = model_;
    p.n_ctx = n_ctx_.load(std::memory_order_acquire);
    p.slots = slots_.load(std::memory_order_acquire);
    p.alive = alive_.load(std::memory_order_acquire);
    p.facts = core::json_obj();
    core::obj_set(p.facts, "base_url", core::json_str(base_url_));
    core::obj_set(p.facts, "owned", core::json_bool(pid_ > 0));
    return p;
}

bool ShadeLlama::run(const TurnRequest& req, const DeltaSink& sink, TurnResult& out, std::string& error) {
    if (!alive_.load(std::memory_order_acquire)) {
        error = "the llama dweller is not answering; this turn was not sent";
        return false;
    }
    abort_.store(false, std::memory_order_release);
    const auto began = now();
    const std::string payload = core::json_dump(llama_body(req, spec_.model_id.empty() ? model_ : spec_.model_id));

    std::string buf;
    std::vector<CallFragment> calls;
    bool saw_content = false;

    const auto on_chunk = [&](std::string_view bytes) -> bool {
        buf.append(bytes);
        std::string event;
        while (take_event(buf, event)) {
            if (event == "[DONE]") continue;
            core::Json chunk;
            try {
                chunk = core::json_parse(event);
            } catch (const std::exception&) {
                continue;   // a fragment of an event that has not finished: keep the bytes
            }
            if (chunk.type != core::Json::Obj) continue;
            const core::Json* choices = core::json_get(chunk, "choices");
            if (choices && choices->type == core::Json::Arr && !choices->arr.empty()) {
                const core::Json& c0 = *choices->arr[0];
                const core::Json* delta = core::json_get(c0, "delta");
                if (delta && delta->type == core::Json::Obj) {
                    Delta d;
                    const core::Json* content = core::json_get(*delta, "content");
                    if (content && content->type == core::Json::Str) d.content = content->str;
                    const core::Json* reasoning = core::json_get(*delta, "reasoning_content");
                    if (reasoning && reasoning->type == core::Json::Str) d.reasoning = reasoning->str;
                    if (!d.content.empty() || !d.reasoning.empty()) {
                        saw_content = true;
                        if (!sink(d)) { abort_.store(true, std::memory_order_release); return false; }
                    }
                    const core::Json* tc = core::json_get(*delta, "tool_calls");
                    if (tc && tc->type == core::Json::Arr) {
                        for (const auto& frag : tc->arr) {
                            if (frag->type != core::Json::Obj) continue;
                            const core::Json* idx = core::json_get(*frag, "index");
                            size_t at = idx && idx->type == core::Json::Num ? static_cast<size_t>(idx->num) : calls.size();
                            if (at >= calls.size()) calls.resize(at + 1);
                            CallFragment& call = calls[at];
                            const core::Json* id = core::json_get(*frag, "id");
                            if (id && id->type == core::Json::Str && !id->str.empty()) call.id = id->str;
                            const core::Json* fn = core::json_get(*frag, "function");
                            if (fn && fn->type == core::Json::Obj) {
                                const core::Json* name = core::json_get(*fn, "name");
                                if (name && name->type == core::Json::Str && !name->str.empty()) call.name = name->str;
                                const core::Json* args = core::json_get(*fn, "arguments");
                                if (args && args->type == core::Json::Str) call.arguments += args->str;
                            }
                        }
                    }
                }
                const core::Json* finish = core::json_get(c0, "finish_reason");
                if (finish && finish->type == core::Json::Str && !finish->str.empty()) out.finish = finish->str;
            }
            const core::Json* usage = core::json_get(chunk, "usage");
            if (usage && usage->type == core::Json::Obj) {
                const core::Json* pt = core::json_get(*usage, "prompt_tokens");
                if (pt && pt->type == core::Json::Num) out.prompt_tokens = static_cast<long>(pt->num);
                const core::Json* ct = core::json_get(*usage, "completion_tokens");
                if (ct && ct->type == core::Json::Num) out.generated = static_cast<long>(ct->num);
                const core::Json* cached = core::json_get(*usage, "cached_tokens");
                if (cached && cached->type == core::Json::Num) out.reused = static_cast<long>(cached->num);
            }
            const core::Json* timings = core::json_get(chunk, "timings");
            if (timings && timings->type == core::Json::Obj) {
                const core::Json* pp = core::json_get(*timings, "prefill_ms");
                if (pp && pp->type == core::Json::Num) out.prompt_ms = pp->num;
                const core::Json* dec = core::json_get(*timings, "decode_ms");
                if (dec && dec->type == core::Json::Num) out.decode_ms = dec->num;
            }
        }
        return !abort_.load(std::memory_order_acquire);   // false ends the transfer: the caller gave up
    };

    const long stall_ms = spec_.silence_s > 0 ? static_cast<long>(spec_.silence_s) * 1000 : 120000;
    const net::Response r = net::post_stream(join_url(base_url_, "/v1/chat/completions"), payload, stall_ms,
                                            &abort_, on_chunk);
    out.prompt_ms = out.prompt_ms > 0 ? out.prompt_ms : static_cast<double>(elapsed_ms(began));

    if (abort_.load(std::memory_order_acquire)) {
        out.finish = "canceled";
        return saw_content;   // a given-up turn with a partial answer is an interrupted turn, not a failure
    }
    if (!r.ok()) {
        error = r.error.empty() ? "the llama dweller answered " + std::to_string(r.status) : r.error;
        alive_.store(false, std::memory_order_release);   // an answer like this means the server is not usable
        return false;
    }
    if (!calls.empty()) {
        core::Json arr = core::json_arr();
        for (auto& call : calls) {
            if (call.name.empty()) continue;   // a fragment with no name is not a call the loop can run
            core::Json one = core::json_obj();
            core::obj_set(one, "id", core::json_str(call.id.empty() ? "call_" + std::to_string(arr.arr.size() + 1) : call.id));
            core::obj_set(one, "type", core::json_str("function"));
            core::Json fn = core::json_obj();
            core::obj_set(fn, "name", core::json_str(call.name));
            core::obj_set(fn, "arguments", core::json_str(call.arguments));
            core::obj_set(one, "function", fn);
            core::arr_push(arr, one);
        }
        if (!arr.arr.empty()) out.tool_calls = arr;
    }
    if (out.finish.empty()) out.finish = saw_content ? "stop" : "error";
    return true;
}

void ShadeLlama::stop() {
    alive_.store(false, std::memory_order_release);
    abort_.store(true, std::memory_order_release);
    if (pid_ <= 0) return;   // adopted, not owned: this process may not end someone else's server
    std::string error;
    const int code = end_process(pid_, 10000, error);
    (void)code;
    pid_ = -1;
}

}  // namespace limbo
}  // namespace dd
