#include "dd/learn/record.hpp"

namespace dd {
namespace learn {

const char* outcome_name(Outcome o) {
    switch (o) {
        case Outcome::Ignored: return "ignored";
        case Outcome::Opened: return "opened";
        case Outcome::Helped: return "helped";
        case Outcome::Failed: return "failed";
        case Outcome::Vetoed: return "vetoed";
    }
    return "?";
}

const char* state_name(State s) {
    switch (s) {
        case State::Draft: return "draft";
        case State::Probation: return "probation";
        case State::Active: return "active";
        case State::Dormant: return "dormant";
        case State::Retired: return "retired";
        case State::Rejected: return "rejected";
    }
    return "?";
}

}  // namespace learn
}  // namespace dd
