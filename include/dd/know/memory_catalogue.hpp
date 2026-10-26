#pragma once
// dd/know/memory_catalogue.hpp — the maze's curated rows as cards, searchable locally.
//
// decision: bug: invariant: ops: fact: status: signal: feedback: user: reference: are the rows a
// person or the harness deliberately wrote down (~3.7k of ~300k). Their labels and first lines are
// cached beside the binary (<home>/cache/memory-cards.jsonl) and synced by id, so a search costs
// microseconds and works with the pod down; opening a row is a primary-key read. Transcripts
// (auto:turn:, auto:reasoning:) are not cards: the session ledger and the pod's recall own those.
//
// The deep leg is the pod's embedding recall (6-17 s, one at a time): the Recaller, kept as it was.

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "dd/context/memory.hpp"
#include "dd/core/snap_cell.hpp"
#include "dd/know/bm25.hpp"
#include "dd/know/catalogue.hpp"
#include "dd/know/event.hpp"

namespace dd {
namespace know {

class MemoryCatalogue : public Catalogue {
public:
    // cache_file: "" keeps nothing on disk
    MemoryCatalogue(std::shared_ptr<context::Memory> mem, std::filesystem::path cache_file = {}, size_t page_bytes = 4200);

    Kind kind() const override { return Kind::Memory; }
    bool ready() const override { return !snap_.empty(); }
    bool wait_ready(std::chrono::milliseconds t) const override { return snap_.wait(t); }
    bool healthy() const override { return healthy_.load(); }
    uint64_t generation() const override;
    size_t size() const override;
    std::vector<Hit> search(const Query& q, int k) const override;
    std::optional<Body> open(const Uri& u) override;
    std::string map() const override;
    void refresh() override;
    // The learn overlay: priors and learned terms for the rows. Like SkillCatalogue, it must be
    // set before the first publish (preload() or refresh()) or the first snapshot ships priors the next pass already dropped.
    void set_overlay(std::shared_ptr<Overlay> o) { overlay_ = std::move(o); }
    // The cached rows, searchable before any pod call (refresh() starts with the same). Call it before the refresher
    // thread starts, or from it: the rows are the refreshing thread's alone.
    void preload();
    void cancel(bool on) override { cancel_.store(on); }
    // prefixes whose last listing hit the 6000-row cap with more behind: those rows are not cards
    size_t truncated() const { return truncated_.load(); }
    float idf(const std::string& term) const override;

    static const std::vector<std::string>& prefixes();
    // "decision:daedalus-cpp-knowledge-router" -> a gist from the row's text
    static std::string gist_of_row(const std::string& content);

private:
    struct Row {
        long id = 0;
        std::string label, gist;
    };
    struct Snapshot {
        std::unique_ptr<Bm25f> index;
        std::map<std::string, size_t> by_group;
        uint64_t generation = 0;
    };
    std::shared_ptr<context::Memory> mem_;
    std::filesystem::path cache_;
    size_t page_bytes_;
    core::SnapCell<Snapshot> snap_;
    std::atomic<bool> healthy_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<size_t> truncated_{0};
    std::atomic<uint64_t> gen_{0};
    std::map<long, Row> rows_;   // the refresh thread's alone
    bool cache_loaded_ = false;

    void load_cache();
    void save_cache() const;
    void publish();
    std::shared_ptr<Overlay> overlay_;
    uint64_t published_overlay_v_ = 0;   // the overlay version the published snapshot was built with
};

}  // namespace know
}  // namespace dd
