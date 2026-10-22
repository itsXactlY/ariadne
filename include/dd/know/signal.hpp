#pragma once
// dd/know/signal.hpp — what the harness tells the router has happened.
//
// The router never looks at the tape or the engine: it is told. A turn started,
// a batch of tool calls finished, the window was rebuilt, the model asked. A
// CallRecord is one call and what came back, boiled down to what retrieval can
// use: the words the model used, the paths and binaries it touched, whether it
// failed and how, whether it was only looking around.

#include <cstddef>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "dd/core/message.hpp"
#include "dd/know/types.hpp"

namespace dd {
namespace know {

struct SessionStarted {};

struct TurnStarted {
    std::string text;             // what the user wrote
    std::string anchored;         // the question as it is looked up: a short one carries the previous real turn
    bool trivial = false;         // "ok", "thanks": nothing to look up
    bool skip_deep = false;       // a rebase just recalled for this question: the pointers only
    int epoch = 0;
    std::set<long> in_window;     // memory ids already on the tape
    std::vector<long> earlier;    // this session's turn ids that are not
};

struct CallRecord {
    std::string tool;             // read_file, run_terminal, skill_load, ...
    std::string intent;           // what the model said it was doing ("" when it did not say)
    std::string args;             // the arguments as sent
    std::string digest;           // tool + normalised arguments: two equal digests are the same call
    std::vector<std::string> paths, binaries;
    bool failed = false;
    bool refused = false;         // a rule held the call: it never ran, so it is neither an error nor a blind look
    int exit = 0;
    std::string error;            // the error line(s) of the result, normalised; "" when none
    bool exploring = false;       // a look around: list, find, grep, ls, cat of a path
    bool empty = false;           // it found nothing
    size_t bytes = 0;             // the result's size
};

struct CallsFinished {
    std::vector<CallRecord> calls;
};

struct Rebased {
    int epoch = 0;
    std::string question;
};

struct Asked {
    std::string query;
    std::vector<Kind> kinds;
    int k = 5;
};

using Signal = std::variant<SessionStarted, TurnStarted, CallsFinished, Rebased, Asked>;

// A call and its result as the router wants to see it.
CallRecord make_record(const core::ToolCall& call, const std::string& result);

// The error signature of a result: its error lines with numbers and long paths folded away, so
// "cannot open /home/x/a.cpp:312" and "cannot open /home/x/b.cpp:77" are the same error.
std::string error_signature(const std::string& text);

}  // namespace know
}  // namespace dd
