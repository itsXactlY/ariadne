#pragma once
// dd/context/render.hpp — what a session message looks like on the wire.
//
// Hygiene runs once, when an epoch starts (Renderer::epoch): the head window,
// finished tool exchanges older than the TTL collapsed into one line that
// names each call, reasoning older than its window dropped (the maze has it)
// or, with the maze dead, moved to disk behind a read_file handle. After
// that a message is rendered once as it enters the tape (Renderer::one):
// tool results capped to their share of the window, the turn's recall on the
// current user message. Port of run_agent.py's _render_payload_message,
// _collapse_aged_tool_groups, _reasoning_payload, _cap_tool_result and
// _window_messages_for_api.

#include <functional>
#include <string>
#include <vector>

#include "dd/context/tape.hpp"
#include "dd/core/message.hpp"

namespace dd {
namespace context {

struct RenderPolicy {
    int window_turns = -1;      // memory.soak.window_turns: head = 2x messages; < 0 keeps all
    int reasoning_window = 3;   // context.reasoning_window: newest reasoning blocks kept
    int tool_group_ttl = 8;     // context.tool_group_ttl: newest tool exchanges kept whole
    long context_length = 0;    // server n_ctx; 0 = results are never capped
};

// Working material on disk, only while the maze is dead:
// <cwd>/.daedalus/spill/<session>/.
class Spill {
public:
    explicit Spill(std::string dir) : dir_(std::move(dir)) {}
    std::string write(const std::string& kind, const std::string& text);   // the path, or ""
    const std::string& dir() const { return dir_; }

private:
    std::string dir_;
    unsigned seq_ = 0;
};

class Renderer {
public:
    using MazeUp = std::function<bool()>;
    using Soak = std::function<void(Item&)>;   // hands an item's reasoning to the maze, once

    Renderer(RenderPolicy p, MazeUp maze_up, Soak soak, Spill* spill)
        : p_(p), maze_up_(std::move(maze_up)), soak_(std::move(soak)), spill_(spill) {}
    RenderPolicy& policy() { return p_; }
    // The same rendering without side effects (no soak, no disk): what a
    // prewarm thread uses to produce the bytes main will send.
    Renderer quiet() const {
        Renderer r = *this;
        r.soak_ = nullptr;
        r.spill_ = nullptr;
        return r;
    }

    struct Epoch {
        std::vector<ItemPtr> sources;           // what the first tape block covers
        std::vector<core::Message> rendered;
    };
    // The first render of an epoch: hygiene over the whole view.
    Epoch epoch(const std::vector<ItemPtr>& items, long current, const std::string& injection);
    // One message as it enters the tape.
    std::vector<core::Message> one(Item& item, bool is_current, const std::string& injection,
                                   const std::vector<ItemPtr>& view);

    // "patch(src/world.cpp)": a dropped call still says what was done.
    static std::string describe_call(const core::ToolCall& c);
    // Fix orphaned tool results / tool calls left by a head cut.
    static std::vector<core::Message> sanitize(std::vector<core::Message> msgs);
    // A tool result's capped text, or "" when it fits as it is.
    std::string capped(const core::Message& tool, const std::vector<ItemPtr>& view) const;
    // Server tokens per chars/4 token, learned from the last call.
    void note_ratio(long real_tokens, long estimated_tokens);
    long budget_chars(int siblings) const;

    static constexpr const char* kResultCut = "[[RESULT CUT";
    static constexpr size_t kExcerptMin = 4000;      // results below this are never cut
    static constexpr size_t kReasoningSpillMin = 1000;

private:
    RenderPolicy p_;
    MazeUp maze_up_;
    Soak soak_;
    Spill* spill_;
    double ratio_ = 1.0;

    std::string excerpt(const std::string& text, const std::string& path, const std::string& tool,
                        long budget, const core::Json* read_args, long total_lines) const;
};

}  // namespace context
}  // namespace dd
