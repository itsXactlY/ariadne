// src/limbo/level.cpp — impl of dd/limbo/level.hpp.
#include "dd/limbo/level.hpp"
#include "dd/limbo/shade_llama.hpp"
#include "dd/limbo/shade_strata.hpp"

#include <exception>
#include <utility>

namespace dd {
namespace limbo {

namespace {

std::string str_of(const core::Json& o, const char* key) {
    const core::Json* v = core::json_get(o, key);
    return v && v->type == core::Json::Str ? v->str : std::string();
}

double num_of(const core::Json& o, const char* key, double fallback) {
    const core::Json* v = core::json_get(o, key);
    return v && v->type == core::Json::Num ? v->num : fallback;
}

long int_of(const core::Json& o, const char* key, long fallback) {
    return static_cast<long>(num_of(o, key, static_cast<double>(fallback)));
}

bool bool_of(const core::Json& o, const char* key, bool fallback) {
    const core::Json* v = core::json_get(o, key);
    return v && v->type == core::Json::Bool ? v->b : fallback;
}

// A refusal's status, read from the dweller's state — never from matching on error text.
int refusal_status(const Minos& m, size_t index) {
    switch (m.state_of(index)) {
        case ShadeState::ADMITTING: return 409;                    // loading; try again later
        case ShadeState::REFUSED: return 503;                      // this level gave up on it
        case ShadeState::DEAD: return 502;                         // it died; the level is not its doctor
        case ShadeState::BUSY:
        case ShadeState::ADMITTED: return m.has_capacity(index) ? 500 : 429;
    }
    return 500;
}

}  // namespace

LimboLevel::LimboLevel(LimboConfig cfg) : cfg_(std::move(cfg)) {
    std::vector<std::unique_ptr<Shade>> built;
    for (const auto& spec : cfg_.shades) {
        ShadeSpec named = spec;
        if (named.name.empty()) named.name = shade_kind_name(named.kind == ShadeKind::None ? ShadeKind::Strata : named.kind);
        if (cfg_.build) { built.push_back(cfg_.build(named)); continue; }
        if (named.kind == ShadeKind::Llama) built.push_back(std::make_unique<ShadeLlama>(named));
        else built.push_back(std::make_unique<ShadeStrata>(named, cfg_.decoder));
    }
    minos_ = std::make_unique<Minos>(std::move(built), cfg_.respawn_budget);
}

LimboLevel::~LimboLevel() { stop(); }

bool LimboLevel::start(std::string& error) {
    std::string notes;
    for (size_t i = 0; i < minos_->count(); ++i) {
        std::string one;
        if (!minos_->admit(i, one)) {
            notes += std::string(minos_->shade(i)->spec().name) + ": " + one + "\n";
            // A dweller that will not come up is reported, and the level still serves with the
            // dwellers that did. Silence about a failed load would be the worse failure.
        }
    }
    front_ = std::make_unique<net::Server>(cfg_.bind, cfg_.port,
                                           [this](const net::Request& r, net::Conn& c) {
                                               ConnWriter w(c);
                                               route(r, w);
                                           },
                                           cfg_.max_body);
    std::string bind_error;
    if (!front_->listen(bind_error)) {
        error = "the limbo level cannot bind " + cfg_.bind + ":" + std::to_string(cfg_.port) + " — " + bind_error;
        front_.reset();
        return false;
    }
    port_ = front_->port();
    serving_ = std::make_unique<std::thread>([this] { front_->serve(stop_); });
    if (!notes.empty()) error = notes;   // non-empty means "started, with these dwellers missing"
    return true;
}

void LimboLevel::stop() {
    stop_.store(1, std::memory_order_release);
    if (serving_ && serving_->joinable()) serving_->join();
    serving_.reset();
    front_.reset();   // the accept socket closes; a handler thread keeps its own conn
    if (minos_) minos_->stop_all();
}

bool LimboLevel::parse_turn(const std::string& body, TurnRequest& out, std::string& error) {
    core::Json b;
    try {
        b = core::json_parse(body);
    } catch (const std::exception& e) {
        error = std::string("the body is not JSON: ") + e.what();
        return false;
    }
    if (b.type != core::Json::Obj) {
        error = "the body is not an object";
        return false;
    }
    out.model_override = str_of(b, "model");
    out.stream = bool_of(b, "stream", false);
    const core::Json* messages = core::json_get(b, "messages");
    if (!messages || messages->type != core::Json::Arr || messages->arr.empty()) {
        error = "this front needs a non-empty `messages` array";
        return false;
    }
    for (const auto& m : messages->arr) {
        if (m->type != core::Json::Obj) {
            error = "a message is not an object";
            return false;
        }
        out.messages.push_back(core::message_from_json(*m));
    }
    Sampling& s = out.sampling;
    s.max_tokens = int_of(b, "max_tokens", -1);
    s.temperature = num_of(b, "temperature", 0.0);
    s.top_k = static_cast<int>(num_of(b, "top_k", -1));
    s.top_p = num_of(b, "top_p", 1.0);
    s.min_p = num_of(b, "min_p", 0.0);
    s.repetition_penalty = num_of(b, "repeat_penalty", num_of(b, "repetition_penalty", 1.0));
    s.frequency_penalty = num_of(b, "frequency_penalty", 0.0);
    s.presence_penalty = num_of(b, "presence_penalty", 0.0);
    s.penalty_last_n = static_cast<int>(num_of(b, "penalty_last_n", s.penalty_last_n));
    s.seed = int_of(b, "seed", -1);
    s.effort = str_of(b, "reasoning_effort");
    if (const core::Json* tools = core::json_get(b, "tools"); tools && tools->type == core::Json::Arr) out.tools = *tools;
    if (const core::Json* stop = core::json_get(b, "stop"); stop) {
        if (stop->type == core::Json::Str) out.stop_seqs.push_back(stop->str);
        else if (stop->type == core::Json::Arr) {
            for (const auto& v : stop->arr)
                if (v->type == core::Json::Str) out.stop_seqs.push_back(v->str);
        }
    }
    return true;
}

core::Json LimboLevel::chunk_json(const Delta& d, const std::string& model, const std::string& finish) {
    core::Json delta = core::json_obj();
    if (!d.content.empty()) core::obj_set(delta, "content", core::json_str(d.content));
    if (!d.reasoning.empty()) core::obj_set(delta, "reasoning_content", core::json_str(d.reasoning));
    core::Json choice = core::json_obj();
    core::obj_set(choice, "index", core::json_num(0));
    core::obj_set(choice, "delta", delta);
    core::obj_set(choice, "finish_reason", finish.empty() ? core::json_null() : core::json_str(finish));
    core::Json choices = core::json_arr();
    core::arr_push(choices, choice);
    core::Json out = core::json_obj();
    core::obj_set(out, "id", core::json_str("limbo"));
    core::obj_set(out, "object", core::json_str("chat.completion.chunk"));
    core::obj_set(out, "model", core::json_str(model));
    core::obj_set(out, "choices", choices);
    return out;
}

// The timings block on its own: a streaming turn puts it in the last chunk, a non-streaming
// one puts it in the answer. Same numbers, one builder — reading a field back out of a freshly
// built answer is a copy through a vector of shared_ptr, not a field.
core::Json LimboLevel::timings_json(const TurnResult& r) {
    core::Json timings = core::json_obj();
    core::obj_set(timings, "prefill_ms", core::json_num(r.prompt_ms));
    core::obj_set(timings, "decode_ms", core::json_num(r.decode_ms));
    core::obj_set(timings, "prompt_read", core::json_num(r.prompt_read));
    core::obj_set(timings, "offloaded", core::json_num(r.offloaded));
    core::obj_set(timings, "drafts_offered", core::json_num(r.drafts_offered));
    core::obj_set(timings, "drafts_accepted", core::json_num(r.drafts_accepted));
    return timings;
}

core::Json LimboLevel::completion_json(const TurnResult& r, const std::string& model) {
    core::Json message = core::json_obj();
    core::obj_set(message, "role", core::json_str("assistant"));
    core::obj_set(message, "content", core::json_str(r.content));
    if (!r.reasoning.empty()) core::obj_set(message, "reasoning_content", core::json_str(r.reasoning));
    if (r.tool_calls.type == core::Json::Arr && !r.tool_calls.arr.empty()) core::obj_set(message, "tool_calls", r.tool_calls);
    core::Json choice = core::json_obj();
    core::obj_set(choice, "index", core::json_num(0));
    core::obj_set(choice, "message", message);
    core::obj_set(choice, "finish_reason", core::json_str(r.finish.empty() ? "stop" : r.finish));
    core::Json choices = core::json_arr();
    core::arr_push(choices, choice);

    core::Json usage = core::json_obj();
    core::obj_set(usage, "prompt_tokens", core::json_num(r.prompt_tokens));
    core::obj_set(usage, "completion_tokens", core::json_num(r.generated));
    core::obj_set(usage, "cached_tokens", core::json_num(r.reused));
    core::obj_set(usage, "total_tokens", core::json_num(r.prompt_tokens + r.generated));

    core::Json out = core::json_obj();
    core::obj_set(out, "id", core::json_str("limbo"));
    core::obj_set(out, "object", core::json_str("chat.completion"));
    core::obj_set(out, "model", core::json_str(model));
    core::obj_set(out, "choices", choices);
    core::obj_set(out, "usage", usage);
    core::obj_set(out, "timings", timings_json(r));
    return out;
}

void LimboLevel::route(const net::Request& req, Writer& out) {
    if (req.method == "GET" && req.path == "/health") {
        core::Json p = minos_->props_json();
        core::obj_set(p, "status", core::json_str("ok"));
        core::obj_set(p, "port", core::json_num(port_));
        core::obj_set(p, "live", core::json_num(live()));
        out.json(200, core::json_dump(p));
        return;
    }
    if (req.method == "GET" && req.path == "/props") {
        out.json(200, core::json_dump(minos_->props_json()));
        return;
    }
    if (req.method == "GET" && req.path == "/slots") {
        out.json(200, core::json_dump(minos_->slots_json()));
        return;
    }
    if (req.method == "GET" && req.path == "/metrics") {
        out.respond(200, "text/plain; version=0.0.4", minos_->metrics());
        return;
    }
    if (req.method == "GET" && (req.path == "/v1/models" || req.path == "/models")) {
        models(out);
        return;
    }
    if (req.method == "POST" && req.path == "/v1/chat/completions") {
        completions(req, out);
        return;
    }
    if (req.method == "POST" && req.path == "/v1/completions") {
        // The completions shape wants a prompt this front has not been given a tokenizer for.
        out.json(400, core::json_dump(Writer::error_json("invalid_request_error",
                                                         "this front speaks Chat-Completions; a raw prompt needs the token seam")));
        return;
    }
    out.json(404, core::json_dump(Writer::error_json("not_found", "the limbo level has no path '" + req.path + "'")));
}

void LimboLevel::models(Writer& out) const {
    core::Json data = core::json_arr();
    for (size_t i = 0; i < minos_->count(); ++i) {
        const Props p = minos_->shade(i)->props();
        if (p.model.empty()) continue;
        core::Json one = core::json_obj();
        core::obj_set(one, "id", core::json_str(p.model));
        core::obj_set(one, "object", core::json_str("model"));
        core::obj_set(one, "owned_by", core::json_str(minos_->shade(i)->spec().name));
        core::obj_set(one, "n_ctx", core::json_num(p.n_ctx));
        core::arr_push(data, one);
    }
    core::Json p = core::json_obj();
    core::obj_set(p, "object", core::json_str("list"));
    core::obj_set(p, "data", data);
    out.json(200, core::json_dump(p));
}

void LimboLevel::completions(const net::Request& req, Writer& out) {
    TurnRequest turn;
    std::string error;
    if (!parse_turn(req.body, turn, error)) {
        out.json(400, core::json_dump(Writer::error_json("invalid_request_error", error)));
        return;
    }

    const Route picked = minos_->route(turn);
    if (!picked.ok) {
        const int status = (!turn.model_override.empty() || turn.kind != ShadeKind::None) ? 404 : 503;
        out.json(status, core::json_dump(Writer::error_json(status == 404 ? "model_not_found" : "unavailable", picked.reason)));
        return;
    }

    const std::string model = minos_->shade(picked.index)->props().model;
    TurnResult result;
    const bool streaming = turn.stream;

    if (streaming) {
        if (!out.stream_begin(200, "text/event-stream")) return;
        const DeltaSink sink = [&](const Delta& d) -> bool {
            if (!out.open()) return false;   // the peer is gone; the dweller is told to stop
            return out.stream_write("data: " + core::json_dump(chunk_json(d, model, "")) + "\n\n");
        };
        const bool ran = minos_->dispatch(picked, turn, sink, result, error);
        if (!out.open()) return;   // no final chunk for a peer that left
        core::Json last = chunk_json(Delta(), model, ran ? result.finish : "error");
        core::Json usage = core::json_obj();
        core::obj_set(usage, "prompt_tokens", core::json_num(result.prompt_tokens));
        core::obj_set(usage, "completion_tokens", core::json_num(result.generated));
        core::obj_set(usage, "cached_tokens", core::json_num(result.reused));
        core::obj_set(last, "usage", usage);
        core::obj_set(last, "timings", timings_json(result));
        out.stream_write("data: " + core::json_dump(last) + "\n\n");
        out.stream_write("data: [DONE]\n\n");
        return;
    }

    // Non-streaming: the deltas are collected, and one answer goes out at the end. The sink
    // still returns false if the connection dies, so a turn nobody is waiting for stops.
    const DeltaSink sink = [&](const Delta& d) -> bool {
        if (!out.open()) return false;
        result.content += d.content;
        result.reasoning += d.reasoning;
        return true;
    };
    const bool ran = minos_->dispatch(picked, turn, sink, result, error);
    if (ran) {
        out.json(200, core::json_dump(completion_json(result, model)));
        return;
    }
    out.json(refusal_status(*minos_, picked.index),
             core::json_dump(Writer::error_json("api_error", error.empty() ? "the dweller refused this turn" : error)));
}

}  // namespace limbo
}  // namespace dd
