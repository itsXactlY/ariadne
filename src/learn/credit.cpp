#include "dd/learn/credit.hpp"

namespace dd {
namespace learn {

Outcome RuleCredit::judge(const Exposure& x) const {
    if (x.vetoed) return Outcome::Vetoed;
    if (!x.opened) return Outcome::Ignored;
    if (x.error_recurred || x.stalled) return Outcome::Failed;
    if (x.trailing_failures >= failed_run) return Outcome::Opened;   // read, and it ended badly without a verdict
    return Outcome::Helped;
}

}  // namespace learn
}  // namespace dd
