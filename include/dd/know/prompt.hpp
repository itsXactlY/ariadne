#pragma once
// dd/know/prompt.hpp — one voice for the system prompt.
//
// The prompt used to have three authors saying opposite things about the same action: the tool
// preamble ("memory is only for history"), the mazemaker server's own protocol pasted in whole
// ("RECALL FIRST, NO EXCEPTIONS", save every turn, session_search, a tool this harness does not
// have) and the harness's paragraph ("already done, call recall only for what is missing"). Sections
// now come from the modules that own them, in one order, and a server's protocol text is a choice:
// instructions: harness (the harness's contract replaces it; the default for the maze), verbatim, off.

#include <string>
#include <vector>

namespace dd {
namespace know {

class PromptComposer {
public:
    PromptComposer& section(const std::string& text);   // empty text is skipped
    std::string str() const;                            // sections, a blank line between

    struct Contract {
        bool memory = true;     // a maze answers: mem:// pointers, mazemaker_remember
        bool skills = true;     // a skill catalogue is there
        bool find = true;       // the find tool is registered
        bool learn = false;     // learning is on, so the close-the-turn line is worth its tokens
    };
    // The harness's side of the memory protocol, in the model's terms. About 130 words.
    static std::string retrieval_contract(const Contract& c);
    // The workflow the harness holds a turn to: plan, act on evidence, verify by running,
    // finish. About 120 words; it says nothing about tools a session does not have.
    static std::string workflow_contract(const Contract& c);
    // "skills 568: devops 149 · ..." lines, under one heading; "" when there are none
    static std::string catalogue_map(const std::vector<std::string>& lines);

    enum class Instructions { Harness, Verbatim, Off };
    static Instructions instructions_mode(const std::string& configured, bool is_maze);
    // what of a server's own instructions goes into the prompt
    static std::string server_instructions(Instructions mode, const std::string& text);

private:
    std::vector<std::string> sections_;
};

}  // namespace know
}  // namespace dd
