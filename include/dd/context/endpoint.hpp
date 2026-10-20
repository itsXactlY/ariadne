#pragma once
// dd/context/endpoint.hpp — the inference server as the tape sees it, and
// what every request tells it.
//
// A local llama-server with -np 2 holds the main conversation on slot 1 and
// the sidekick (prewarm, background work) on slot 0: it fills its shared
// decode batch in slot order, so the small sidekick prompts go first and the
// big main prefills never starve them. Strata holds one conversation at a
// time and parks the other. Main calls carry their slot, n_cache_reuse (a
// spliced middle is relocated, not re-read) and the thinking budget. Port of run_agent.py's slot pinning + agent/kv_watermarks.py's
// server half + agent/backend_kind.py.

#include <atomic>
#include <string>
#include <vector>

#include "dd/core/json.hpp"

namespace dd {
namespace context {

struct ServerInfo {
    bool local = false;      // 127.0.0.1 / localhost: slots, erase and probes apply
    bool reachable = false;  // /props answered
    bool strata = false;     // one conversation at a time: parks instead of slots
    int slots = 1;
    long n_ctx = 0;
    std::string chat_template, build;
};

class Endpoint {
public:
    explicit Endpoint(std::string base_url);
    const std::string& base_url() const { return base_url_; }
    std::string root() const { return root_; }   // "" for a remote provider

    const ServerInfo& probe();                   // /props + /slots
    const ServerInfo& info() const { return info_; }
    bool multi_slot() const { return info_.local && info_.slots >= 2; }
    // One conversation at a time (a one-slot Strata): a second one parks the
    // first, so a prewarm is parked and cancellable, never beside main.
    bool single_sequence() const { return info_.local && info_.strata && info_.slots < 2; }

    int main_slot() const;
    int sidekick_slot() const;
    void promote(int slot);                      // main moved onto a prewarmed slot

    struct Slot {
        int id = 0;
        long tokens = 0;
        bool busy = false;
    };
    std::vector<Slot> slots() const;             // GET /slots, empty on failure
    // Main's tokens plus what the sidekick still holds: under a unified KV
    // cache both share the fast layer.
    long pool_tokens(long main_tokens) const;
    bool erase(int slot) const;                  // needs --slot-save-path on llama-server

    // The fields a request carries. thinking: reasoning is on for this call.
    void shape(core::Json& body, int slot, bool thinking) const;
    int thinking_budget = 1024;                  // agent.thinking_budget; 0 = no cap

    // The effort ladder this model's chat template implements ("off" first);
    // the whole ladder when there is no template to read, nothing when the
    // template has no reasoning switch at all.
    std::vector<std::string> efforts() const;

private:
    std::string base_url_, root_;
    ServerInfo info_;
    std::atomic<int> main_{1};   // a prewarm thread reads it while main may be promoted
};

inline constexpr const char* kThinkingBudgetMessage = "\n\nThinking budget reached. Act on what is known now.\n";

}  // namespace context
}  // namespace dd
