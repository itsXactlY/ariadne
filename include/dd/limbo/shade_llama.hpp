#pragma once
// dd/limbo/shade_llama.hpp — the llama dweller: llama.cpp-adaptive-kv-streaming, over HTTP.
//
// This is the fallback, and it stays a server: its own chat template, its own tokenizer, its
// own slots, its adaptive KV streaming over the PCIe seam. The level proxies one turn to it and
// hands the event stream back as deltas — so a consumer of the limbo level cannot tell which
// dweller answered.
//
// Two ways to be a dweller:
//   spawned  — the level runs llama-server itself on `port`, with `args`, and ends it on stop().
//   adopted  — `exe` is empty and `base_url` names a server someone else started. The level
//              probes it, uses it, and never ends it: a server this process did not spawn is
//              not this process's to kill.
//
// Giving up on a turn has no protocol verb here: llama.cpp stops producing when its client
// goes away. So interrupt() flips the flag the transfer is watching, and the stream ends.

#include <atomic>
#include <string>
#include <sys/types.h>
#include <vector>

#include "dd/limbo/shade.hpp"

namespace dd {
namespace limbo {

// The request body for one turn, in the shape llama-server reads. Exported so a gate can
// compare it against what the harness sends over HttpLLM for the same messages.
core::Json llama_body(const TurnRequest& req, const std::string& model);

class ShadeLlama : public Shade {
public:
    explicit ShadeLlama(ShadeSpec spec);
    ~ShadeLlama() override;

    const ShadeSpec& spec() const override { return spec_; }
    bool start(std::string& error) override;
    bool alive() const override { return alive_.load(std::memory_order_acquire); }
    Props props() const override;
    bool run(const TurnRequest& req, const DeltaSink& sink, TurnResult& out, std::string& error) override;
    void interrupt() override { abort_.store(true, std::memory_order_release); }
    void stop() override;

    std::string url() const { return base_url_; }
    bool owns_server() const { return pid_ > 0; }

private:
    // Read /props once, and say what the dweller reports. Called at start and whenever asked.
    bool probe(std::string& error);

    ShadeSpec spec_;
    std::string base_url_;
    pid_t pid_ = -1;
    std::atomic<bool> alive_{false};
    std::atomic<bool> abort_{false};
    std::atomic<int> n_ctx_{0};
    std::atomic<int> slots_{1};
    std::string model_;      // what the server names, read from /props
};

}  // namespace limbo
}  // namespace dd
