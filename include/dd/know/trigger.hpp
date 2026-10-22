#pragma once
// dd/know/trigger.hpp — when to look something up: the "when" of retrieval, as pure functions.
//
// A trigger reads a signal and the working set and plans retrievals (an Intent: which catalogues,
// what query, how much room, where it lands). It does no I/O and changes nothing, so every rule
// is a unit test. The router runs the plan.
//
//   TurnStartTrigger  a question arrived: skills, memory and tools by what was asked, plus the pod's recall
//   ShiftTrigger      after a tool batch the task moved: terms the turn has not covered (two or more)
//   FailureTrigger    a call failed: what the maze knows about that error
//   StallTrigger      the same call twice, or three looks around that found nothing: poking blind

#include <functional>
#include <memory>
#include <vector>

#include "dd/know/policy.hpp"
#include "dd/know/signal.hpp"
#include "dd/know/text.hpp"
#include "dd/know/working_set.hpp"

namespace dd {
namespace know {

struct PlanContext {
    const WorkingSet& ws;
    const Policy& policy;
    const Distiller& distill;
    IdfFn idf;   // the best idf any catalogue gives a term
};

class Trigger {
public:
    virtual ~Trigger() = default;
    virtual const char* name() const = 0;
    virtual void plan(const Signal& s, const PlanContext& c, Plan& out) const = 0;
};

class TurnStartTrigger : public Trigger {
public:
    const char* name() const override { return "turn"; }
    void plan(const Signal& s, const PlanContext& c, Plan& out) const override;
};

class ShiftTrigger : public Trigger {
public:
    const char* name() const override { return "shift"; }
    void plan(const Signal& s, const PlanContext& c, Plan& out) const override;
};

class FailureTrigger : public Trigger {
public:
    const char* name() const override { return "failure"; }
    void plan(const Signal& s, const PlanContext& c, Plan& out) const override;
};

class StallTrigger : public Trigger {
public:
    const char* name() const override { return "stall"; }
    void plan(const Signal& s, const PlanContext& c, Plan& out) const override;
    // the notice's words: "calls ... found nothing" / "the same call twice"
    static std::string notice_text(const WorkingSet& ws, const Policy& p);
};

// stall, failure, shift for a batch; the turn start for a question
std::vector<std::unique_ptr<Trigger>> default_triggers();

// a question about the past ("what did we decide", "last time", "wie war"): the turn waits for the pod's recall
bool asks_about_the_past(const std::string& text);

}  // namespace know
}  // namespace dd
