#include "dd/learn/service.hpp"

#include <ctime>

namespace fs = std::filesystem;

namespace dd {
namespace learn {

int64_t LearnService::now() const { return static_cast<int64_t>(std::time(nullptr)); }

std::unique_ptr<LearnService> LearnService::start(ServiceOptions opt, std::string* err) {
    std::error_code ec;
    if (fs::exists(opt.dir / "OFF", ec)) {
        if (err) *err = "learning is off (" + (opt.dir / "OFF").string() + ")";
        return nullptr;
    }
    std::unique_ptr<LearnService> s(new LearnService());
    s->opt_ = std::move(opt);
    s->ledger_ = Ledger::open(s->opt_.dir, s->opt_.ledger, err);
    if (!s->ledger_) return nullptr;
    s->sink_ = std::make_shared<QueueSink>();
    s->overlay_ = s->opt_.overlay ? s->opt_.overlay : std::make_shared<LearnOverlay>();
    s->curator_ = std::make_unique<Curator>(s->overlay_);
    LearnService* self = s.get();
    s->observer_ = std::make_unique<Observer>([self](OutcomeRec&& o) {
        self->ledger_->record(Record{std::move(o)});
        ++self->outcomes_;
    });
    s->detectors_.push_back(std::make_unique<FixedAfterFailure>());
    s->detectors_.push_back(std::make_unique<LongSuccess>());
    auto pin = std::make_unique<Pinned>();
    s->pinned_ = pin.get();
    s->detectors_.push_back(std::move(pin));
    Reviewer::Options ro;
    ro.home_dir = s->opt_.home_dir;
    ro.project_dir = s->opt_.project_dir;
    s->reviewer_ = std::make_unique<Reviewer>(s->opt_.search ? s->opt_.search : Reviewer::SearchFn([](const know::Query&) { return std::vector<know::Hit>(); }), ro);
    s->skill_sink_ = std::make_unique<SkillSink>(s->opt_.skills_root);
    s->author_ = TemplateAuthor(s->opt_.home_dir);
    s->next_curate_ = s->now();   // the first pass folds what the journal recovered
    s->th_ = std::thread([self] { self->run(); });
    return s;
}

void LearnService::stop() {
    if (!th_.joinable()) return;
    stop_.store(true, std::memory_order_release);
    sink_->wake();
    th_.join();
}

void LearnService::curate_now() {
    curate_.store(true, std::memory_order_release);
    sink_->wake();
}

bool LearnService::wait_idle(std::chrono::milliseconds timeout) const {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (processed_.load(std::memory_order_acquire) < sink_->pushed()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

LearnService::Status LearnService::status() const {
    Status s;
    s.pushed = sink_->pushed();
    s.dropped = sink_->dropped();
    s.processed = processed_.load();
    s.outcomes = outcomes_.load();
    s.drafts = drafts_.load();
    s.rejected = rejected_.load();
    s.passes = passes_.load();
    s.overlay_version = overlay_->version();
    s.ledger = ledger_->health();
    return s;
}

// Only what the fold uses is journaled: the calls, turns and rebases feed the recorder and the observer and are not kept.
void LearnService::handle(const know::Event& e) {
    using T = know::Event::Type;
    if (opt_.feedback) {
        if (e.type == T::Offered || e.type == T::OpenedByRouter || e.type == T::OpenedByModel || e.type == T::Shadow)
            ledger_->record(Record{EventRec{e}});
        observer_->feed(e);
    }
    if (opt_.author) {
        recorder_.feed(e);
        if (e.type == T::TurnEnd) author_now(e.turn);
    }
}

void LearnService::author_now(uint32_t turn) {
    const auto det = best_detection(detectors_, recorder_.window());
    if (!det) return;
    recorder_.drop_through(det->end_turn, det->end_round);
    const int64_t t = now();
    if (t / 86400 != day_) {
        day_ = t / 86400;
        day_drafts_ = 0;
    }
    if (day_drafts_ >= opt_.max_drafts_per_day) return;
    const auto draft = author_.author(*det);
    if (!draft) return;
    const Verdict v = reviewer_->review(*draft);
    know::Uri u;
    u.kind = know::Kind::Skill;
    u.id = draft->name;
    if (!v.ok) {
        ledger_->record(Record{skill_sink_->reject(*draft, v)});
        ++rejected_;
        return;
    }
    DraftRec rec = skill_sink_->write(*draft, v);
    ledger_->record(Record{rec});
    if (rec.accepted) {
        ledger_->record(Record{StateRec{u, State::Draft, State::Probation, "template: " + det->kind, t}});
        ++day_drafts_;
        ++drafts_;
        if (opt_.on_skill) opt_.on_skill();
    } else {
        ++rejected_;
    }
    (void)turn;
}

void LearnService::curate() {
    ledger_->tick();
    if (opt_.feedback) {
        curator_->pass(ledger_->live(), now());
        curator_->write_report(ledger_->live(), now(), opt_.dir);
    }
    ++passes_;
    next_curate_ = now() + opt_.curate_every.count();
}

void LearnService::run() {
    know::Event e;
    while (!stop_.load(std::memory_order_acquire)) {
        bool any = false;
        while (sink_->pop(e)) {
            handle(e);
            processed_.fetch_add(1, std::memory_order_release);
            any = true;
        }
        if (any) ledger_->tick();
        if (curate_.exchange(false) || now() >= next_curate_) curate();
        const int64_t wait_s = std::max<int64_t>(1, next_curate_ - now());
        sink_->wait(std::chrono::seconds(std::min<int64_t>(wait_s, 30)));
    }
    while (sink_->pop(e)) {
        handle(e);
        processed_.fetch_add(1, std::memory_order_release);
    }
    observer_->flush();
    curate();
    ledger_->sync();
}

}  // namespace learn
}  // namespace dd
