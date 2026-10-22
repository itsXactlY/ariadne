#include "dd/learn/observer.hpp"

#include <algorithm>

namespace dd {
namespace learn {

namespace {
know::Uri base(const know::Uri& u) {
    know::Uri k;
    k.kind = u.kind;
    k.id = u.id;
    return k;
}
}  // namespace

Exposure* Observer::find(const know::Uri& u) {
    const know::Uri k = base(u);
    for (auto& x : open_)
        if (x.uri == k) return &x;
    return nullptr;
}

void Observer::close(size_t i) {
    Exposure x = std::move(open_[i]);
    open_.erase(open_.begin() + static_cast<std::ptrdiff_t>(i));
    x.ts = now_;
    OutcomeRec r;
    r.uri = x.uri;
    r.outcome = credit_->judge(x);
    r.trigger = x.trigger;
    r.weight = x.weight();
    r.terms = std::move(x.terms);
    r.episode = x.episode;
    r.ts = now_;
    if (emit_) emit_(std::move(r));
}

void Observer::close_where(const std::function<bool(const Exposure&)>& pred) {
    for (size_t i = 0; i < open_.size();) {
        if (pred(open_[i])) close(i);
        else ++i;
    }
}

void Observer::flush() {
    while (!open_.empty()) close(0);
}

void Observer::feed(const know::Event& e) {
    using T = know::Event::Type;
    if (e.ts_ns) now_ = std::max(now_, static_cast<int64_t>(e.ts_ns / 1000000000ull));
    switch (e.type) {
        case T::TurnStart:
            flush();
            ++episode_;
            round_ = 0;
            break;
        case T::TurnEnd:
            for (auto& x : open_) x.turn_ended = true;
            flush();
            break;
        case T::Offered: {
            round_ = std::max(round_, e.round);
            if (find(e.uri)) break;   // already open: one exposure per card at a time
            if (e.turn != batch_turn_ || e.round != batch_round_) batch_turn_ = e.turn, batch_round_ = e.round, batch_n_ = 0;
            Exposure x;
            x.uri = base(e.uri);
            x.trigger = e.reason;
            x.key = e.key;
            x.terms = e.terms;
            x.turn = e.turn;
            x.round_start = e.round;
            x.episode = episode_;
            x.rank = batch_n_++;
            open_.push_back(std::move(x));
            break;
        }
        case T::OpenedByRouter:
        case T::OpenedByModel: {
            round_ = std::max(round_, e.round);
            const bool model = e.type == T::OpenedByModel;
            Exposure* x = find(e.uri);
            if (!x) {   // opened without a pointer: the router's auto-open, or the model found it itself
                Exposure n;
                n.uri = base(e.uri);
                n.trigger = model ? "model" : e.reason;
                n.key = e.key;
                n.terms = e.terms;
                n.turn = e.turn;
                n.round_start = e.round;
                n.episode = episode_;
                open_.push_back(std::move(n));
                x = &open_.back();
            }
            x->opened = true;
            x->by_model = x->by_model || model;
            break;
        }
        case T::Calls: {
            if (e.round > round_) {
                round_ = e.round;
                close_where([&](const Exposure& x) { return round_ - x.round_start >= window_; });
            }
            if (e.call.refused) break;
            for (auto& x : open_) {
                if (e.call.failed) {
                    ++x.trailing_failures;
                    if (!x.key.empty() && e.call.error == x.key) x.error_recurred = true;
                } else {
                    x.trailing_failures = 0;
                }
            }
            break;
        }
        case T::Stall:
            for (auto& x : open_)
                if (x.opened) x.stalled = true;
            break;
        case T::Tripwire: {
            Exposure x;
            x.uri = base(e.uri);
            x.trigger = "tripwire";
            x.turn = e.turn;
            x.round_start = e.round;
            x.episode = episode_;
            x.vetoed = true;
            open_.push_back(std::move(x));
            close(open_.size() - 1);
            break;
        }
        case T::Rebase:
        case T::Shadow: break;   // the fold counts shadows; a rebase does not end a window
    }
}

}  // namespace learn
}  // namespace dd
