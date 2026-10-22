#pragma once
// dd/know/tripwire.hpp — rules that cost nothing until a pattern fires (oh-my-pi's TTSR, as a gate).
//
// A rule is a file (<home>/rules/*.md) or a maze row (rule:<name>): a regex on what a tool call is
// about to do, and the text to tell the model when it matches. The gate sits before execution: a
// matching call is not run, its result is the rule's text, and the model (which now knows) calls
// again. Spoken once per session by default, so the retry passes; `repeat_gap` lets it speak again
// after that many turns. Until a rule fires, the prompt is byte-for-byte the same with or without it.
//
//   ---
//   name: podman-not-docker
//   description: podman only
//   condition: '(^|[\s;&|(])docker\b'      # a leading (?i) means case-insensitive
//   scope: tool:run_terminal                # tool (any call) | tool:<name>
//   once: true                              # false: every matching call is refused
//   repeat_gap: 20                          # turns before it may speak again (with once: true)
//   ---
//   This box runs podman, never docker. ...

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "dd/agent/agent.hpp"
#include "dd/context/memory.hpp"

namespace dd {
namespace know {

struct Rule {
    std::string name, description, text, source;   // source: the file, or "maze"
    std::string condition;                         // as written
    std::string tool;                              // "" any tool, else only this one
    bool once = true;
    int repeat_gap = 0;                            // turns; 0 with once: never again
};

class Tripwires {
public:
    Tripwires() = default;

    // false (and why) on a bad condition; a rule of the same name replaces the earlier one
    bool add(Rule r, std::string* why = nullptr);
    size_t load_dir(const std::filesystem::path& dir);        // rules/*.md; returns how many loaded
    size_t load_maze(context::Memory& mem);                    // rule:* rows
    size_t size() const { return rules_.size(); }
    std::vector<std::string> names() const;
    void set_clock(std::function<int()> turn) { turn_ = std::move(turn); }

    // The rules this call trips and has not yet spoken to; marks them spoken. Agent thread only.
    std::vector<const Rule*> check(const core::ToolCall& call);
    // the tool result that refuses the call
    static std::string veto_text(const std::vector<const Rule*>& rules);

    // text -> rule; false and why when it has no condition or an unusable one
    static bool parse(const std::string& file_name, const std::string& text, Rule* out, std::string* why);
    // The operator's first rules, installed by `dd_tui --init --rules`: (file name, content)
    static std::vector<std::pair<std::string, std::string>> starter_rules();

private:
    struct Compiled {
        Rule rule;
        std::regex re;
        int spoken_turn = -1000000;
        bool spoken = false;
    };
    std::vector<Compiled> rules_;
    std::function<int()> turn_;
};

// The agent's gate over a Tripwires.
class TripwireGate : public agent::ToolGate {
public:
    explicit TripwireGate(Tripwires& t) : trip_(t) {}
    std::optional<std::string> veto(const core::ToolCall& call) override;
    std::function<void(const std::string& note)> on_fire;   // "⛔ rule podman-not-docker held podman ..."

private:
    Tripwires& trip_;
};

}  // namespace know
}  // namespace dd
