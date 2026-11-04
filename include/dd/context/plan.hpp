#pragma once
// dd/context/plan.hpp — the ledger as a node in the maze.
//
// The todo ledger was a run's property: it lived in the process, and the process that died
// took the plan with it. A plan belongs to the work, not to the window — so every state the
// ledger reaches is written to the maze as a revision of its own, ariadne:plan:<session>:<rev>,
// and any later run reads the newest one back: a fresh start, /clear, /resume, a rebase whose
// turn 0 restates the list. That is the difference between a task list and a task: the list
// can be lost with the window, the task cannot be lost with the process.
//
// Revisions are append-only and never rewritten, so the newest is the current state — the same
// convention the tape uses for the same list, for the same reason: an append is whole or
// nothing, and a reader can tell which state it is looking at from the node alone.
//
// A node is written for a reader that has none of this code: what the plan is, which revision,
// whether everything is closed, and the items with their statuses — so a plain
// mazemaker_recall finds a task's plan and knows what it is, and a plan left open is still a
// plan in the graph the dream cycles walk.

#include <string>
#include <vector>

#include "dd/context/memory.hpp"
#include "dd/tools/todo_tool.hpp"

namespace dd {
namespace context {

// The namespace every plan revision lives in. One label walk answers "what plan is open".
extern const char* const kPlanNamespace;   // "ariadne:plan:"

// One revision of a plan, as the maze holds it.
struct PlanDoc {
    std::string session;
    int rev = 0;
    long long unix_s = 0;
    bool done = false;          // every item closed: the plan is history, not work
    long id = 0;                // the node it was written as, or read from
    std::vector<tools::TodoItem> items;
};

// pending + in_progress: what is still the plan's to do.
int plan_open(const std::vector<tools::TodoItem>& items);

// ariadne:plan:<session>:<rev>, the revision zero-padded so a label walk sorts the way numbers do.
std::string plan_label(const std::string& session, int rev);
// The session and revision a plan label names; false for anything else in the namespace.
bool plan_label_split(const std::string& label, std::string* session, int* rev);

// The node's body, and its reader. plan_parse reads a body this code wrote; anything else is
// not a plan and says so rather than handing back a half list.
std::string plan_content(const PlanDoc& d);
bool plan_parse(const std::string& content, PlanDoc* out);

// The "- [>] id. text (status)" lines of a ledger rendering (todo_tool's injection, or a plan
// body): one format the tool, the tape and the maze all agree on.
bool plan_parse_items(const std::string& text, std::vector<tools::TodoItem>* out);
// The open items of a ledger rendering as one line, for a recall query: what the session is
// working on, which the question alone does not say.
std::string plan_open_text(const std::string& ledger_text, size_t max_chars);

// The writer: the session it mirrors, the revision it is at, and the state it has already
// handed over — so a turn boundary that restates an unchanged ledger costs nothing.
class PlanLog {
public:
    // A new session: its revisions start at one, and the list is written again under it (the
    // label is the session's, so the same list is a new node in a new session).
    void session(const std::string& id);
    const std::string& session() const { return session_; }
    int rev() const { return rev_; }
    // A plan read back from the maze: the next change is one revision above it, and the state
    // just read is not written again. The mirror's own session stands: the plan moves to it.
    void adopt(const PlanDoc& d);
    // true: the ledger moved. Fills *d with the revision that says so. An empty list is a
    // state too (the ledger was emptied): it closes the plan instead of leaving a stale one
    // open in the maze forever.
    bool moved(const std::vector<tools::TodoItem>& items, long long now_s, PlanDoc* d);

private:
    std::string session_, last_;
    int rev_ = 0;
};

// ---- the read side ----
// The newest revision of one session's plan. A label walk and one primary-key read: no
// embedding, so it answers at boot, before anything else is warm.
bool plan_of(Memory& mem, const std::string& session, PlanDoc* out);
// The newest revision of any session's plan that still has work in it.
bool plan_newest_open(Memory& mem, PlanDoc* out);

}  // namespace context
}  // namespace dd
