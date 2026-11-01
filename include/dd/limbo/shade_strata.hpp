#pragma once
// dd/limbo/shade_strata.hpp — the Strata dweller: the packed-MoE engine over a pipe.
//
// This shade is not a fallback. llama.cpp cannot serve a packed expert arena, a TQ1_0 trellis
// MMQ, expert-profile routing, resident experts, int8 KV residency or an MTP draft, so for
// these models the engine IS the inference path and Charon is the only crossing to it.
//
// The engine takes pre-tokenized ids and says nothing about text: it hands back ids. So this
// shade needs a Decoder — the BPE lexicon — and until one is wired it refuses by name. A
// refusal is better than a half-answer that shows a turn as ids.
//
// Sampling rides the same line as the prompt, in the engine's own spelling (temperature=…,
// top_p=…, top_k=… clamped to the engine's 64-candidate ceiling, min_p=…, penalty_repeat=…
// with a window, seed=…, and the per-turn tuning keys pcie_frac / spec_min_p). Absent means
// greedy; temperature 0 means the same thing and is not forwarded.

#include <string>
#include <vector>

#include "dd/limbo/charon.hpp"
#include "dd/limbo/shade.hpp"

namespace dd {
namespace limbo {

// The engine's sampling keys for one turn, in its spelling. Exported so a gate can compare
// the line against what serve/server.py::sampling_keys produces for the same request.
std::string sampling_keys(const Sampling& s);

// One request line: `GEN <max_new>[keys] id,id,...`. `max_new` -1 becomes 0 — the engine
// reads the prompt and makes no token, which is what a caller asking for nothing wants.
std::string gen_line(const TurnRequest& req);

class ShadeStrata : public Shade {
public:
    ShadeStrata(ShadeSpec spec, const Decoder* decoder);

    const ShadeSpec& spec() const override { return spec_; }
    bool start(std::string& error) override;
    bool alive() const override { return pipe_.alive(); }
    Props props() const override;
    // Text needs the lexicon. An id-only shade given a message turn is a turn that cannot be
    // answered, so routing must see the capability, not guess it from a model name.
    bool speaks_text() const override { return decoder_ != nullptr; }
    bool run(const TurnRequest& req, const DeltaSink& sink, TurnResult& out, std::string& error) override;
    void interrupt() override { pipe_.stop_generation(); }
    void stop() override { pipe_.stop(); }

    const Charon& pipe() const { return pipe_; }

private:
    ShadeSpec spec_;
    const Decoder* decoder_;
    Charon pipe_;
};

}  // namespace limbo
}  // namespace dd
