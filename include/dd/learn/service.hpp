#pragma once
// dd/learn/service.hpp — the one object the app builds to make the router learn.
//
// It owns the ledger, the thread and everything that thread runs: the observer (events become
// outcomes), the curator (outcomes become priors and learned terms) and the authoring chain
// (episodes become skills). The agent thread only ever pushes onto sink(). Quitting stops the
// thread first (declare the service after the router and the catalogues).

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dd/know/types.hpp"
#include "dd/learn/author.hpp"
#include "dd/learn/curator.hpp"
#include "dd/learn/detector.hpp"
#include "dd/learn/episode.hpp"
#include "dd/learn/ledger.hpp"
#include "dd/learn/observer.hpp"
#include "dd/learn/reviewer.hpp"
#include "dd/learn/sink.hpp"
#include "dd/learn/skill_sink.hpp"

namespace dd {
namespace learn {

struct ServiceOptions {
    std::filesystem::path dir;           // <home>/learn
    std::filesystem::path skills_root;   // <home>/skills
    std::string home_dir, project_dir;   // for the reviewer's private-path rule
    bool feedback = true, author = true;
    std::chrono::seconds curate_every{600};
    int max_drafts_per_day = 3;
    Reviewer::SearchFn search;           // the catalogue, for the duplicate check
    std::function<void()> on_skill;      // a skill was written: ask the catalogue to refresh
    LedgerOptions ledger;
};

class LearnService {
public:
    // nullptr when the ledger cannot be opened (err says why) or <dir>/OFF exists.
    static std::unique_ptr<LearnService> start(ServiceOptions opt, std::string* err = nullptr);
    ~LearnService() { stop(); }
    LearnService(const LearnService&) = delete;
    LearnService& operator=(const LearnService&) = delete;

    std::shared_ptr<know::EventSink> sink() const { return sink_; }
    std::shared_ptr<know::Overlay> overlay() const { return overlay_; }
    void pin() { pinned_->arm(); }       // /learn: the next turn end turns the recent work into a skill
    void stop();

    struct Status {
        uint64_t pushed = 0, dropped = 0, processed = 0, outcomes = 0, drafts = 0, rejected = 0, passes = 0;
        uint64_t overlay_version = 0;
        Ledger::Health ledger;
    };
    Status status() const;
    void curate_now();                   // tests and `dd learn`: run a pass on the learn thread soon
    bool wait_idle(std::chrono::milliseconds timeout) const;   // every pushed event processed (tests)

private:
    LearnService() = default;
    void run();
    void handle(const know::Event& e);
    void author_now(uint32_t turn);
    void curate();
    int64_t now() const;

    ServiceOptions opt_;
    std::unique_ptr<Ledger> ledger_;
    std::shared_ptr<QueueSink> sink_;
    std::shared_ptr<LearnOverlay> overlay_;
    std::unique_ptr<Curator> curator_;
    std::unique_ptr<Observer> observer_;
    EpisodeRecorder recorder_;
    std::vector<std::unique_ptr<Detector>> detectors_;
    Pinned* pinned_ = nullptr;           // owned by detectors_
    TemplateAuthor author_;
    std::unique_ptr<Reviewer> reviewer_;
    std::unique_ptr<SkillSink> skill_sink_;
    std::thread th_;
    std::atomic<bool> stop_{false}, curate_{false};
    std::atomic<uint64_t> processed_{0}, outcomes_{0}, drafts_{0}, rejected_{0}, passes_{0};
    int64_t next_curate_ = 0, day_ = 0;
    int day_drafts_ = 0;
};

}  // namespace learn
}  // namespace dd
