#pragma once
// dd/know/tool_catalogue.hpp — the deferred tools as cards: an adapter over the ToolDeck.
//
// The tools in the window need no pointer. The long tail of MCP tools behind tool_search does: a
// question about BTQuant can be answered with "tool: btquant_run_backtest (deferred)" before the
// model has to guess a name. Opening one is the deck's own tool_describe.

#include <atomic>
#include <map>
#include <memory>
#include <string>

#include "dd/context/deck.hpp"
#include "dd/core/snap_cell.hpp"
#include "dd/know/bm25.hpp"
#include "dd/know/catalogue.hpp"

namespace dd {
namespace know {

class ToolCatalogue : public Catalogue {
public:
    explicit ToolCatalogue(const context::ToolDeck& deck) : deck_(deck) {}

    Kind kind() const override { return Kind::Tool; }
    bool ready() const override { return !snap_.empty(); }
    bool wait_ready(std::chrono::milliseconds t) const override { return snap_.wait(t); }
    uint64_t generation() const override { return snap_.empty() ? 0 : 1; }
    size_t size() const override;
    std::vector<Hit> search(const Query& q, int k) const override;
    std::optional<Body> open(const Uri& u) override;
    std::string map() const override;
    void refresh() override;   // the deck is arranged once, at launch
    float idf(const std::string& term) const override;

private:
    struct Snapshot {
        std::unique_ptr<Bm25f> index;
        std::map<std::string, size_t> by_source;
    };
    const context::ToolDeck& deck_;
    core::SnapCell<Snapshot> snap_;
};

}  // namespace know
}  // namespace dd
