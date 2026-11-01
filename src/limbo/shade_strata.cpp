// src/limbo/shade_strata.cpp — impl of dd/limbo/shade_strata.hpp.
#include "dd/limbo/shade_strata.hpp"

#include <chrono>
#include <format>
#include <string>
#include <vector>

namespace dd {
namespace limbo {

namespace {

// Python's repr of a float and std::format's general form agree: both are the shortest
// round-trip decimal. The engine parses the value, not the spelling, so an agreement that
// costs nothing is worth having.
std::string num(double v) { return std::format("{}", v); }

std::chrono::steady_clock::time_point now() { return std::chrono::steady_clock::now(); }

long elapsed_ms(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(now() - since).count();
}

}  // namespace

std::string sampling_keys(const Sampling& s) {
    std::string keys;
    if (s.temperature > 0.0) keys += " temperature=" + num(s.temperature);
    if (s.top_p < 1.0) keys += " top_p=" + num(s.top_p);
    if (s.top_k >= 0) {
        // The engine's sampled path keeps at most 64 candidates: 0 ("off") and any wider
        // request gets all 64, so a wider number is not an error — it is the ceiling.
        keys += " top_k=" + std::to_string(s.top_k >= 1 && s.top_k <= 64 ? s.top_k : 64);
    }
    if (s.min_p > 0.0 && s.min_p <= 1.0) keys += " min_p=" + num(s.min_p);
    const bool rp_on = s.repetition_penalty != 1.0;
    const bool pf_on = s.frequency_penalty != 0.0;
    const bool pp_on = s.presence_penalty != 0.0;
    if (rp_on) keys += " penalty_repeat=" + num(s.repetition_penalty);
    if (pf_on) keys += " penalty_freq=" + num(s.frequency_penalty);
    if (pp_on) keys += " penalty_present=" + num(s.presence_penalty);
    if (rp_on || pf_on || pp_on) {
        // A penalty with no window counts over nothing, so the window rides with it.
        keys += " penalty_last_n=" + std::to_string(s.penalty_last_n > 0 ? s.penalty_last_n : 64);
    }
    if (s.seed > 0) keys += " seed=" + std::to_string(s.seed);
    // Per-turn tuning, measured without a restart: the PCIe fraction and the draft's floor.
    if (s.pcie_frac >= 0.0 && s.pcie_frac <= 1.0) keys += " pcie_frac=" + num(s.pcie_frac);
    if (s.spec_min_p >= 0.0 && s.spec_min_p <= 1.0) keys += " spec_min_p=" + num(s.spec_min_p);
    return keys;
}

std::string gen_line(const TurnRequest& req) {
    const long max_new = req.sampling.max_tokens < 0 ? 0 : req.sampling.max_tokens;
    std::string line = "GEN " + std::to_string(max_new) + sampling_keys(req.sampling);
    line += " ";
    for (size_t i = 0; i < req.ids.size(); ++i) {
        if (i) line += ",";
        line += std::to_string(req.ids[i]);
    }
    return line;
}

ShadeStrata::ShadeStrata(ShadeSpec spec, const Decoder* decoder)
    : spec_(std::move(spec)), decoder_(decoder), pipe_(spec_) {}

bool ShadeStrata::start(std::string& error) {
    if (!pipe_.start(error)) return false;
    if (spec_.n_ctx > 0 && pipe_.max_context() != spec_.n_ctx) {
        // A window smaller than the level asked for is a lie about the dweller, not a detail:
        // Minos judges a prompt against the ceiling, and the ceiling here is the engine's.
        error = spec_.exe + " loaded a " + std::to_string(pipe_.max_context()) + " token window, this level asked for " +
                std::to_string(spec_.n_ctx);
        pipe_.stop();
        return false;
    }
    return true;
}

Props ShadeStrata::props() const {
    Props p;
    p.kind = ShadeKind::Strata;
    p.model = spec_.model_id;
    p.n_ctx = pipe_.max_context() > 0 ? pipe_.max_context() : spec_.n_ctx;
    p.facts = pipe_.facts();
    p.alive = pipe_.alive();
    if (p.facts.type == core::Json::Obj) {
        const core::Json* slots = core::json_get(p.facts, "slots");
        if (slots && slots->type == core::Json::Num) p.slots = static_cast<int>(slots->num);
    }
    return p;
}

bool ShadeStrata::run(const TurnRequest& req, const DeltaSink& sink, TurnResult& out, std::string& error) {
    if (!pipe_.alive()) {
        error = "the strata dweller is not crossed; this turn was not sent";
        return false;
    }
    if (decoder_ == nullptr) {
        error = "the strata dweller has no lexicon: ids cannot be spoken";
        return false;
    }
    if (req.ids.empty()) {
        error = "the turn carries no ids";
        return false;
    }
    if (!pipe_.send(gen_line(req), error)) return false;

    const long silence_ms = spec_.silence_s > 0 ? static_cast<long>(spec_.silence_s) * 1000 : 0;
    // Until the first line the ceiling is longer: a prompt read at the slowest rate is not a
    // frozen engine, and a frozen one still dies (#481's reading, as a wall clock).
    auto heard = now();
    long allowance = silence_ms > 0 ? silence_ms + static_cast<long>(req.ids.size()) * 4 : 0;
    bool stopped = false;
    bool gave_up = false;

    while (true) {
        EngineLine el;
        const long left = allowance > 0 ? std::min<long>(allowance - elapsed_ms(heard), 1000) : 1000;
        if (!pipe_.next(el, left > 0 ? left : 1)) {
            if (pipe_.dead()) {
                error = "the strata dweller ended mid-turn (exit code " +
                        std::to_string(pipe_.exit_code()) + ")";
                return false;
            }
            if (req.cancel && req.cancel->load(std::memory_order_acquire)) {
                // Quiet because nobody is producing is indistinguishable from quiet because
                // the caller stopped caring; the caller's own flag says which.
                pipe_.stop_generation();
                stopped = true;
                gave_up = true;
                heard = now();
                allowance = silence_ms;
                continue;
            }
            error = "the strata dweller said nothing for " + std::to_string(silence_ms / 1000) + " s";
            return false;
        }
        heard = now();
        allowance = silence_ms;

        switch (el.kind) {
            case LineKind::Token: {
                if (el.token < 0) break;
                const int id = static_cast<int>(el.token);
                if (decoder_->stop_token(id)) {
                    pipe_.stop_generation();
                    stopped = true;
                    out.finish = "stop";
                    break;
                }
                Delta d;
                d.content = decoder_->piece(id);
                // false: the reader went away. STOP the dweller and end this turn — generating
                // on for a peer nobody is reading is tokens spent on nobody.
                if (!sink(d)) {
                    pipe_.stop_generation();
                    stopped = true;
                    gave_up = true;
                    break;
                }
                if (!stopped && req.cancel && req.cancel->load(std::memory_order_acquire)) {
                    pipe_.stop_generation();
                    stopped = true;
                    gave_up = true;
                }
                break;
            }
            case LineKind::Resume:
                out.reused = el.n;
                break;
            case LineKind::Done: {
                if (el.fields.type == core::Json::Obj) {
                    auto take_int = [&](const char* key, long& into) {
                        const core::Json* v = core::json_get(el.fields, key);
                        if (v && v->type == core::Json::Num) into = static_cast<long>(v->num);
                    };
                    auto take_double = [&](const char* key, double& into) {
                        const core::Json* v = core::json_get(el.fields, key);
                        if (v && v->type == core::Json::Num) into = v->num;
                    };
                    take_int("generated", out.generated);
                    take_int("prompt_tokens", out.prompt_tokens);
                    take_double("prompt_ms", out.prompt_ms);
                    take_double("decode_ms", out.decode_ms);
                    take_int("drafts_offered", out.drafts_offered);
                    take_int("drafts_accepted", out.drafts_accepted);
                    take_int("prompt_read", out.prompt_read);
                    take_int("offloaded", out.offloaded);
                    const core::Json* reused = core::json_get(el.fields, "reused");
                    if (reused && reused->type == core::Json::Num) out.reused = static_cast<long>(reused->num);
                    const core::Json* finish = core::json_get(el.fields, "finish");
                    if (finish && finish->type == core::Json::Str) out.finish = finish->str;
                }
                if (stopped) out.finish = gave_up ? "canceled" : out.finish;
                if (out.finish.empty()) out.finish = gave_up ? "canceled" : "stop";
                return true;
            }
            case LineKind::Err: {
                const core::Json* reason = core::json_get(el.fields, "reason");
                error = reason && reason->type == core::Json::Str ? reason->str : el.raw;
                // ERR means the engine refused and is idle: no DONE follows, so there is
                // nothing to drain. The next request needs no clean-up.
                return false;
            }
            case LineKind::Closed:
                error = "the strata dweller ended mid-turn (exit code " + std::to_string(pipe_.exit_code()) + ")";
                return false;
            case LineKind::Progress:
                break;   // the prompt is being read; the level reads progress from /props
            default:
                break;   // INFO / LP / batch lines: kept in the raw line, and in the log
        }
    }
}

}  // namespace limbo
}  // namespace dd
