#pragma once
// dd/context/deck.hpp — which tools sit in the window.
//
// Local tools and tool_search.always_load stay visible; every other tool
// (the MCP servers' long tail) is reached through three bridge tools:
// tool_search finds it, tool_describe loads its schema, tool_call runs it.
// The tools array never changes during a session, so it stays part of the
// cached prefix — a discovered tool is called through the bridge instead of
// being added to the array. Port of tools/tool_search.py's registry half
// (classify, assemble, dispatch) over the ported BM25 engine.

#include <functional>
#include <set>
#include <string>
#include <vector>

#include "dd/core/json.hpp"
#include "dd/tools/registry.hpp"
#include "dd/tools/tool_search.hpp"

namespace dd {
namespace context {

class ToolDeck {
public:
    ToolDeck(tools::ToolRegistry& reg, tools::ToolSearchConfig cfg) : reg_(reg), cfg_(std::move(cfg)) {}

    // core: tools that are never deferred (the harness's own). Registers the
    // bridge tools when deferral is on.
    void arrange(const std::set<std::string>& core);
    core::Json definitions() const { return defs_; }   // the tools array every request carries
    std::function<void(const tools::ToolEntry&)> registrar;   // how the bridges get registered (default: plainly)

    bool active() const { return active_; }
    int visible() const { return static_cast<int>(visible_.size()); }
    int deferred() const { return static_cast<int>(deferred_.size()); }
    long deferred_tokens() const { return deferred_tokens_; }
    const std::vector<std::string>& deferred_names() const { return deferred_; }

    std::string search(const core::Json& args) const;
    std::string describe(const core::Json& args) const;
    std::string call(const core::Json& args) const;

private:
    tools::ToolRegistry& reg_;
    tools::ToolSearchConfig cfg_;
    std::vector<std::string> visible_, deferred_;
    std::vector<tools::CatalogEntry> catalog_;
    core::Json defs_ = core::json_arr();
    long deferred_tokens_ = 0;
    bool active_ = false;

    bool deferrable(const std::string& name) const;
};

}  // namespace context
}  // namespace dd
