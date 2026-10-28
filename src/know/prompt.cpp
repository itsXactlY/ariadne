// src/know/prompt.cpp — impl of dd/know/prompt.hpp.
#include "dd/know/prompt.hpp"

#include <algorithm>
#include <cctype>

namespace dd {
namespace know {

PromptComposer& PromptComposer::section(const std::string& text) {
    const size_t b = text.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return *this;
    sections_.push_back(text.substr(b, text.find_last_not_of(" \t\r\n") - b + 1));
    return *this;
}

std::string PromptComposer::str() const {
    std::string out;
    for (const auto& s : sections_) out += (out.empty() ? "" : "\n\n") + s;
    return out;
}

std::string PromptComposer::retrieval_contract(const Contract& c) {
    std::string s =
        "## Retrieval\n"
        "The harness looks things up for you and attaches <retrieval> notes: pointers" +
        std::string(c.skills && c.memory ? " to skills and memories" : c.skills ? " to skills" : c.memory ? " to memories" : "") +
        ", never answers. A note after a tool result means the task shifted or a call failed.\n";
    if (c.skills || c.memory) {
        s += "- A pointer that fits the task MUST be opened before you act on it:";
        if (c.skills) s += " read_file(\"skill://name\")";
        if (c.skills && c.memory) s += ",";
        if (c.memory) s += " read_file(\"mem://id\")";
        s += ". A long skill continues with #2, #3.\n";
    }
    s += "- NEVER guess a path, a command or a name. Not sure where something is:";
    s += c.find ? " find(\"what you are looking for\") first, then read what it returns.\n" : " look with list_dir or a search command first, then read.\n";
    s += "- Live state (GPU, RAM, files, ports, the running model): run the command. Memory is history and may be out of date.\n";
    s += std::string("- The same call twice with the same result means stop") + (c.find ? ": find(), then change the approach.\n" : ": change the approach.\n");
    if (c.memory)
        s += "- The harness already writes every finished turn to the maze. Save only distilled facts with mazemaker_remember "
             "(labels decision:, bug:, fact:, invariant:, ops:, status:).\n";
    return s;
}

std::string PromptComposer::workflow_contract(const Contract& c) {
    std::string s =
        "## Workflow\n"
        "- A task with more than one step gets a plan first: write it with `todo`, a few items saying what will change, then work it in order.\n"
        "- The ledger travels as tool output, not as a standing block: you see it in the `todo` result, and after a rebase it comes back in the anchor above the working context. Re-read it there; re-write it if it is gone.\n"
        "- Facts come from running, not guessing. Read the file, run the command, look at what it actually returned.\n"
        "- A change is not done when it compiles; it is done when the thing that changed was run and its result was looked at.\n"
        "- Fix the cause. Suppressing an error or special-casing an input to make a symptom disappear is not a fix.\n"
        "- Independent steps run together; steps that depend on an earlier result run in order.\n"
        "- Skills are guidelines you read and adapt, not a ritual to follow exactly.\n";
    if (c.learn) s += "- Close a turn by recording what is true now: the decision, the path, the number — not a retelling of the work.\n";
    return s;
}

std::string PromptComposer::catalogue_map(const std::vector<std::string>& lines) {
    std::string body;
    for (const auto& l : lines)
        if (!l.empty()) body += "- " + l + "\n";
    if (body.empty()) return "";
    return "Catalogues you can search:\n" + body;
}

PromptComposer::Instructions PromptComposer::instructions_mode(const std::string& configured, bool is_maze) {
    std::string c;
    for (char ch : configured) c.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    if (c == "harness") return Instructions::Harness;
    if (c == "verbatim") return Instructions::Verbatim;
    if (c == "off" || c == "false" || c == "none") return Instructions::Off;
    return is_maze ? Instructions::Harness : Instructions::Verbatim;
}

std::string PromptComposer::server_instructions(Instructions mode, const std::string& text) {
    return mode == Instructions::Verbatim ? text : std::string();
}

}  // namespace know
}  // namespace dd
