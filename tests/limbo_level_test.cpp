// tests/limbo_level_test.cpp — the limbo level's front, against a fake dweller and a captured
// writer: no socket, because the paths and the status codes are what this file is about.
//
// Asserted, in one line each:
//   every path the Python endpoint spoke still answers, and nothing else does;
//   a text turn comes back as a Chat-Completions answer, streamed or whole, with the figures;
//   a body that is not a turn is refused as 400, and an unadmitted model as 404;
//   a dweller that dies mid-turn answers as an error, never as an empty completion;
//   a peer that goes away mid-stream stops the dweller.
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "dd/limbo/level.hpp"
#include "fake_shade.hpp"

using namespace dd;
using namespace dd::limbo;

namespace {
int g_fail = 0;
void chk(bool ok, const std::string& what) {
    if (!ok) { ++g_fail; std::printf("FAIL %s\n", what.c_str()); }
    else std::printf("ok   %s\n", what.c_str());
}

// The captured front: what a handler wrote, in order, and whether the peer is still there.
class Capture : public Writer {
public:
    bool respond(int status, std::string_view ct, std::string_view body) override {
        ++responses_;
        status_ = status;
        content_type_ = ct;
        body_ = body;
        return open_;
    }
    bool stream_begin(int status, std::string_view ct) override {
        ++responses_;
        status_ = status;
        content_type_ = ct;
        streaming_ = true;
        return open_;
    }
    bool stream_write(std::string_view bytes) override {
        body_ += bytes;
        ++events_;
        return open_;
    }
    bool open() const override { return open_; }
    void close_peer() { open_ = false; }

    int status() const { return status_; }
    int responses() const { return responses_; }
    int events() const { return events_; }
    const std::string& body() const { return body_; }
    const std::string& content_type() const { return content_type_; }
    bool streaming() const { return streaming_; }

private:
    bool open_ = true;
    int status_ = 0;
    int responses_ = 0;
    int events_ = 0;
    bool streaming_ = false;
    std::string body_, content_type_;
};

net::Request req(const std::string& method, const std::string& path, const std::string& body = "") {
    net::Request r;
    r.method = method;
    r.path = path;
    r.target = path;
    r.body = body;
    return r;
}

// A fake lexicon: enough for routing to see that strata can speak text, not enough to decode
// anything real. The level does not own a tokenizer in P0.
struct PieceLexicon : Decoder {
    std::string piece(int id) const override { return id == 0 ? std::string() : "id" + std::to_string(id); }
    bool stop_token(int id) const override { return id == 0; }
};

PieceLexicon g_lexicon;

LimboConfig config(bool with_lexicon) {
    LimboConfig c;
    ShadeSpec strata;
    strata.kind = ShadeKind::Strata;
    strata.name = "strata";
    strata.model_id = "moe";
    strata.n_ctx = 4096;
    ShadeSpec llama;
    llama.kind = ShadeKind::Llama;
    llama.name = "llama";
    llama.model_id = "dense";
    llama.port = 1;   // stands for a parallel server: props().slots is 2
    c.shades = {strata, llama};
    c.decoder = with_lexicon ? static_cast<const Decoder*>(&g_lexicon) : nullptr;
    c.build = [](const ShadeSpec& s) { return std::make_unique<FakeShade>(s); };
    c.port = 0;
    return c;
}

core::Json parsed(const std::string& body) { return core::json_parse(body); }

std::string message_content(const core::Json& answer) {
    const core::Json* choices = core::json_get(answer, "choices");
    if (!choices || choices->type != core::Json::Arr || choices->arr.empty()) return std::string();
    const core::Json* message = core::json_get(*choices->arr[0], "message");
    if (!message) return std::string();
    const core::Json* content = core::json_get(*message, "content");
    return content && content->type == core::Json::Str ? content->str : std::string();
}
}  // namespace

int main() {
    // Unbuffered: an assert that crashes must still show every line it printed before the crash.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // --- the paths, and only the paths ---
    {
        LimboLevel level(config(false));
        Capture out;
        level.route(req("GET", "/health"), out);
        chk(out.status() == 200, "/health answers");
        chk(core::json_get(parsed(out.body()), "status")->str == "ok", "/health says ok");
        chk(core::json_get(parsed(out.body()), "live") != nullptr, "/health carries the live connection count");
        chk(core::json_get(parsed(out.body()), "port") != nullptr, "/health names the port it bound");

        Capture props;
        level.route(req("GET", "/props"), props);
        chk(props.status() == 200 && props.content_type() == "application/json", "/props answers as JSON");
        chk(core::json_get(parsed(props.body()), "n_ctx")->num == 4096, "/props says the window the dwellers report");
        chk(core::json_get(parsed(props.body()), "dwellers")->arr.size() == 2, "/props lists both dwellers");

        Capture slots;
        level.route(req("GET", "/slots"), slots);
        chk(slots.status() == 200 && core::json_get(parsed(slots.body()), "slots")->arr.size() == 2, "/slots names both dwellers");
        chk(core::json_get(*core::json_get(parsed(slots.body()), "slots")->arr[0], "state") != nullptr,
            "each slot carries its state");

        Capture metrics;
        level.route(req("GET", "/metrics"), metrics);
        chk(metrics.status() == 200 && metrics.content_type().find("text/plain") != std::string::npos, "/metrics answers as text");
        chk(metrics.body().find("# TYPE limbo_shade_turns_total counter") != std::string::npos, "/metrics declares its types");

        Capture models;
        level.route(req("GET", "/v1/models"), models);
        chk(models.status() == 200, "/v1/models answers");
        chk(core::json_get(parsed(models.body()), "data")->arr.size() == 2, "both dwellers are listed");
        chk(core::json_get(*core::json_get(parsed(models.body()), "data")->arr[0], "id")->str == "moe",
            "the alias is the dweller's own name");

        Capture gone;
        level.route(req("GET", "/v1/chat/completions"), gone);   // GET, not POST: not a turn
        chk(gone.status() == 404, "a path the level does not speak is a 404, said plainly");
        chk(core::json_get(parsed(gone.body()), "error")->type == core::Json::Obj, "an error body is an error object");

        Capture completions;
        level.route(req("POST", "/v1/completions", "{}"), completions);
        chk(completions.status() == 400, "a raw prompt is refused by name, not guessed at");
    }

    // --- a whole (non-streaming) turn ---
    {
        LimboLevel level(config(true));
        std::string up;
        chk(level.start(up), "the level comes up: " + up);
        chk(level.listening(), "the front is bound");
        chk(level.minos().admitted() == 2, "both dwellers are admitted before anything is asked of them");
        Capture out;
        level.route(req("POST", "/v1/chat/completions",
                        "{\"model\":\"moe\",\"stream\":false,\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}"), out);
        chk(out.status() == 200, "a text turn answers 200");
        const core::Json j = parsed(out.body());
        chk(core::json_get(j, "object")->str == "chat.completion", "the answer is a chat.completion");
        chk(core::json_get(*core::json_get(*core::json_get(j, "choices")->arr[0], "message"), "role")->str == "assistant",
            "the message is the assistant's");
        chk(message_content(j) == "strata-answer", "the pieces assembled into the answer");
        chk(core::json_get(*core::json_get(j, "usage"), "completion_tokens")->num == 2, "the figures ride along");
        chk(core::json_get(*core::json_get(j, "usage"), "cached_tokens")->num == 4, "reused tokens are reported as cached");
        chk(core::json_get(*core::json_get(j, "timings"), "prefill_ms")->num == 3.0, "timings come from the dweller");
        chk(level.minos().slot(0).turns.load() == 1, "the turn is counted on the dweller's slot");
        chk(level.minos().slot(0).tokens.load() == 2, "the tokens it produced are counted too");
        level.stop();
        chk(level.minos().admitted() == 0, "stop puts every dweller back to dead");
    }

    // --- a streamed turn ---
    {
        LimboLevel level(config(true));
        std::string up;
        chk(level.start(up), "the level comes up for a stream: " + up);
        Capture out;
        level.route(req("POST", "/v1/chat/completions",
                        "{\"model\":\"moe\",\"stream\":true,\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}"), out);
        chk(out.status() == 200 && out.streaming(), "a streamed turn opens a stream");
        chk(out.content_type() == "text/event-stream", "the stream is an event stream");
        chk(out.body().find("data: ") == 0, "the first thing out is an event");
        chk(out.body().find("[DONE]\n\n") != std::string::npos, "the stream ends with [DONE]");
        chk(out.body().find("\"content\":\"strata\"") != std::string::npos, "the pieces arrive as deltas");
        chk(out.body().find("\"content\":\"-answer\"") != std::string::npos, "every piece is forwarded, none is held");
        chk(out.body().find("\"usage\"") != std::string::npos, "the last chunk carries the figures");
        chk(out.events() >= 4, "each piece is its own event");
        chk(level.minos().slot(0).turns.load() == 1, "a streamed turn is counted once, not per piece");
        level.stop();
    }

    // --- a peer that goes away mid-stream: nothing is written for it ---
    {
        LimboLevel level(config(true));
        std::string up;
        chk(level.start(up), "the level comes up: " + up);
        Capture out;
        out.close_peer();   // the peer is gone before the turn begins
        level.route(req("POST", "/v1/chat/completions",
                        "{\"model\":\"moe\",\"stream\":true,\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}"), out);
        chk(out.responses() == 1, "a stream opened for a peer that left is one response");
        chk(out.body().find("[DONE]") == std::string::npos, "no [DONE] is written for a peer that is gone");
    }

    // --- bodies that are not turns ---
    {
        LimboLevel level(config(true));
        std::string up;
        chk(level.start(up), "the level comes up: " + up);
        Capture broken;
        level.route(req("POST", "/v1/chat/completions", "{\"messages\":["), broken);
        chk(broken.status() == 400, "a truncated body is refused, not attempted");
        chk(core::json_get(parsed(broken.body()), "error")->type == core::Json::Obj, "the refusal names itself");

        Capture empty;
        level.route(req("POST", "/v1/chat/completions", "{\"messages\":[]}"), empty);
        chk(empty.status() == 400 &&
                core::json_get(*core::json_get(parsed(empty.body()), "error"), "message")->str.find("messages") != std::string::npos,
            "an empty message list is refused by name");

        Capture unknown_model;
        level.route(req("POST", "/v1/chat/completions",
                        "{\"model\":\"nothing-here\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}"), unknown_model);
        chk(unknown_model.status() == 404, "an unadmitted model is a 404, never a substitute");
        level.stop();
    }

    // --- a dweller that dies mid-turn answers as an error, never as an empty completion ---
    {
        LimboLevel level(config(true));
        std::string up;
        chk(level.start(up), "the level comes up: " + up);
        auto* fake = static_cast<FakeShade*>(level.minos().shade(0));
        fake->set_mode("die");
        Capture out;
        level.route(req("POST", "/v1/chat/completions",
                        "{\"model\":\"moe\",\"stream\":false,\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}"), out);
        chk(out.status() >= 500, "a death mid-turn is a server-side error, not an empty answer");
        chk(core::json_get(parsed(out.body()), "error")->type == core::Json::Obj, "the error is an error object");
        chk(core::json_get(parsed(out.body()), "choices") == nullptr, "no choices are offered for a turn that did not run");
        chk(level.minos().state_of(0) == ShadeState::ADMITTED, "the level brought the dweller back before answering");
        chk(level.minos().slot(0).respawns.load() >= 1, "the respawn is counted, not hidden");
        chk(level.minos().slot(0).failed.load() >= 1, "the turn that died is counted as failed");
        level.stop();
    }

    // --- a dweller with no lexicon cannot be handed a text turn ---
    {
        LimboLevel level(config(false));   // no lexicon: strata cannot speak
        std::string up;
        chk(level.start(up), "the level comes up: " + up);
        static_cast<FakeShade*>(level.minos().shade(0))->set_speaks_text(false);   // strata without its lexicon
        chk(!level.minos().shade(0)->speaks_text(), "the strata dweller cannot speak text");
        Capture out;
        level.route(req("POST", "/v1/chat/completions",
                        "{\"model\":\"moe\",\"stream\":false,\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}"), out);
        chk(out.status() == 404, "a text turn for a lexicon-less dweller is refused");
        chk(core::json_get(*core::json_get(parsed(out.body()), "error"), "message")->str.find("lexicon") != std::string::npos,
            "the refusal says the lexicon is missing");

        Capture fallback;
        level.route(req("POST", "/v1/chat/completions", "{\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}"), fallback);
        chk(fallback.status() == 200, "with no model named, the dweller that can speak answers");
        chk(message_content(parsed(fallback.body())) == "llama-answer", "the fallback dweller is the one that answered");
        level.stop();
    }

    // --- parse_turn: the sampling fields, exactly once each ---
    {
        TurnRequest t;
        std::string error;
        const std::string body = "{\"model\":\"moe\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"},"
                                 "{\"role\":\"assistant\",\"content\":\"yo\",\"tool_calls\":[{\"id\":\"c1\","
                                 "\"type\":\"function\",\"function\":{\"name\":\"f\",\"arguments\":\"{}\"}}]}],"
                                 "\"stream\":true,\"max_tokens\":320,\"temperature\":0.6,\"top_k\":40,\"top_p\":0.9,"
                                 "\"min_p\":0.05,\"repeat_penalty\":1.05,\"seed\":7,\"reasoning_effort\":\"none\","
                                 "\"stop\":[\"END\"],\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"f\"}}]}";
        chk(LimboLevel::parse_turn(body, t, error), "a Chat-Completions body parses as a turn");
        chk(t.messages.size() == 2 && t.messages[1].tool_calls.size() == 1, "the tool call survives the wire");
        chk(t.messages[1].tool_calls[0].name == "f", "the tool call's name is read");
        chk(t.sampling.max_tokens == 320 && t.sampling.temperature == 0.6 && t.sampling.top_k == 40, "sampling fields land");
        chk(t.sampling.top_p == 0.9 && t.sampling.min_p == 0.05 && t.sampling.repetition_penalty == 1.05, "the rest land too");
        chk(t.sampling.seed == 7 && t.sampling.effort == "none", "the seed and the effort land");
        chk(t.stream, "stream is read");
        chk(t.stop_seqs.size() == 1 && t.stop_seqs[0] == "END", "stop sequences land");
        chk(t.tools.type == core::Json::Arr && !t.tools.arr.empty(), "the tool set rides to the dweller");
        chk(t.model_override == "moe", "the named model is kept for routing");

        TurnRequest defaults;
        chk(LimboLevel::parse_turn("{\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}", defaults, error),
            "a body with no sampling keys parses");
        chk(defaults.sampling.max_tokens == -1 && defaults.sampling.top_k == -1 && defaults.sampling.seed == -1,
            "absent keys stay the dweller's own defaults, not values this code invented");
        chk(!LimboLevel::parse_turn("not json", defaults, error), "a body that is not JSON is refused");
        chk(!error.empty(), "and the reason is said");
    }

    // --- the answer shape a Chat-Completions reader parses, from the builders ---
    {
        TurnResult r;
        r.finish = "length";
        r.content = "abc";
        r.reasoning = "think";
        r.generated = 3;
        r.prompt_tokens = 12;
        r.reused = 5;
        r.prompt_ms = 1.5;
        r.decode_ms = 2.5;
        r.drafts_offered = 4;
        r.drafts_accepted = 2;
        const core::Json j = LimboLevel::completion_json(r, "moe");
        chk(core::json_get(j, "model")->str == "moe", "the answer names the model that answered");
        chk(core::json_get(*core::json_get(j, "choices")->arr[0], "finish_reason")->str == "length", "a length finish is reported");
        chk(core::json_get(*core::json_get(j, "usage"), "total_tokens")->num == 15, "total is prompt plus completion");
        chk(core::json_get(*core::json_get(j, "timings"), "drafts_accepted")->num == 2, "the draft's hit rate rides the answer");
        chk(core::json_get(*core::json_get(*core::json_get(j, "choices")->arr[0], "message"), "reasoning_content")->str == "think",
            "reasoning goes out under the name the wire reads");

        Delta d;
        d.content = "tok";
        const core::Json chunk = LimboLevel::chunk_json(d, "moe", "");
        chk(core::json_get(chunk, "object")->str == "chat.completion.chunk", "a chunk is a chunk");
        chk(core::json_get(*core::json_get(chunk, "choices")->arr[0], "finish_reason")->type == core::Json::Null,
            "a chunk in flight has no finish reason");
        const core::Json last = LimboLevel::chunk_json(Delta(), "moe", "stop");
        chk(core::json_get(*core::json_get(last, "choices")->arr[0], "finish_reason")->str == "stop",
            "the last chunk closes the turn");
    }

    std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "passed", g_fail);
    return g_fail ? 1 : 0;
}
