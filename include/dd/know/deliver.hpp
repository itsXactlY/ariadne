#pragma once
// dd/know/deliver.hpp — what a note looks like to the model, and what it costs.
//
// A note is pointers (one line per card: kind, name, score, what it is for, how to open it) and,
// for a skill the router is sure of, the head of its text opened for the model. Everything is
// counted in server tokens (chars/4 at the ratio the server measured) against the room the signal
// was given: a note that does not fit is cut at a line, never in the middle of one.

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "dd/know/types.hpp"
#include "dd/know/working_set.hpp"

namespace dd {
namespace know {

struct Shown {
    std::vector<Hit> skills, memory, tools;
    std::optional<Body> head;   // the opened head of the best skill
    std::string head_title;
};

class Deliverer {
public:
    using Ratio = std::function<double()>;   // server tokens per chars/4
    explicit Deliverer(Ratio r = {}) : ratio_(std::move(r)) {}

    size_t tokens(const std::string& text) const;
    // <retrieval why="..."> pointers + an opened head </retrieval>; "" when there is nothing to say.
    // budget: tokens for the pointers; total_budget: for the whole note, an opened head included.
    // offered / head_in (optional) say which cards made it in.
    std::string note(const Shown& s, const std::string& why, size_t budget, size_t total_budget, std::vector<Uri>* offered = nullptr,
                     bool* head_in = nullptr) const;
    // <system-notice reason="..."> text </system-notice>
    static std::string notice(const std::string& reason, const std::string& text);
    // The skills the model was working from before the window was rebuilt, as pointers.
    static std::string reattach(const std::vector<WorkingSet::Entry>& in_use);
    static std::string pointer(const Hit& h);        // one line
    static std::string read_hint(const Uri& u);      // read_file("skill://x"), tool_describe("x")

private:
    Ratio ratio_;
};

}  // namespace know
}  // namespace dd
