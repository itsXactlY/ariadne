#pragma once
// dd/context/tape.hpp — the session's history and the append-only request
// tape rendered from it.
//
// Session keeps every message the turn produced, for the turn soak and the
// rebase; it is never sent whole. Tape freezes what was sent: the first call
// of an epoch renders the whole view with every hygiene layer applied, after
// that a message is rendered once, the first time it enters the payload, and
// every later call reuses that rendering byte for byte. llama-server and
// Strata reuse a prompt only up to the first changed token, so the previous
// request stays an exact prefix of the next. A new epoch starts only when the
// history under the tape is replaced (a rebase), where one prefill is due
// anyway. Port of agent/payload_tape.py + the payload half of run_agent.py.

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "dd/core/message.hpp"

namespace dd {
namespace context {

struct Item {
    core::Message msg;
    bool hidden = false;   // harness-built (a rebase anchor), not a user turn
    bool soaked = false;   // its reasoning went to the maze (render thread only)
};
using ItemPtr = std::shared_ptr<Item>;

class Session {
public:
    explicit Session(std::string id) : id_(std::move(id)) {}
    static std::string new_id();   // dd-<time>-<random>, unique per process start

    const std::string& id() const { return id_; }
    const std::vector<ItemPtr>& items() const { return items_; }
    int generation() const { return generation_; }

    ItemPtr append(core::Message m, bool hidden = false);
    // A rebase: the history under the tape is replaced, a new epoch begins.
    void replace(std::vector<ItemPtr> items);
    // Index of the last real user message, -1 when there is none.
    long current_user() const;

private:
    std::string id_;
    std::vector<ItemPtr> items_;
    int generation_ = 0;
};

class Tape {
public:
    struct Block {
        std::vector<ItemPtr> sources;
        std::vector<core::Message> rendered;
    };

    explicit operator bool() const { return !blocks_.empty(); }
    ItemPtr first_source() const;
    int generation() const { return generation_; }
    size_t messages() const;     // rendered messages on the tape

    void reset();
    void start(std::vector<ItemPtr> sources, std::vector<core::Message> rendered, int generation);
    // (blocks, sources) of the longest tape prefix `view` still starts with.
    // Identity, not equality: a rebase copies, and a copy is another history.
    std::pair<size_t, size_t> matched(const std::vector<ItemPtr>& view, int generation) const;
    void truncate(size_t blocks);
    void append(ItemPtr source, std::vector<core::Message> rendered);
    std::vector<core::Message> payload() const;

private:
    std::vector<Block> blocks_;
    int generation_ = -1;
};

}  // namespace context
}  // namespace dd
