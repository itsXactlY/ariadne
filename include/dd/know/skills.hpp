#pragma once
// dd/know/skills.hpp — the skill catalogue: sources behind one merged, searchable set.
//
//   DiskSkills  SKILL.md trees on disk (./skills first, then read-only external dirs)
//   MazeSkills  cards skill:<source>:<name> and bodies skillsrc:<source>:<name>#NNN in the maze
//   SkillCatalogue  merges them: the first source to name a skill keeps the bare name; an
//                   identical copy (same sha256) collapses into it; the maze's snapshot of a disk
//                   skill (maze:local) yields to the disk copy; any other differing copy stays
//                   reachable as name@source; a card with no body borrows an identical copy's.
//                   Notes and fragments that merely sit under skill: are not skills.
//
// A skill opens a page at a time (skill://name, skill://name#2, skill://name#all) and
// its own files (skill://name/references/x.md) from sources that have them.

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

namespace dd {
namespace know {

class SkillSource {
public:
    virtual ~SkillSource() = default;
    virtual std::string name() const = 0;
    virtual std::vector<Card> list() = 0;                     // blocks; every skill this source holds
    virtual bool ok() const { return true; }                  // did the last list() reach its store?
    virtual size_t skipped() const { return 0; }              // rows in the last list() that were not skills
    virtual void cancel(bool /*on*/) {}                       // abandon a list() in progress
    virtual std::optional<std::string> body(const Card& c) = 0;                                // the SKILL.md
    virtual std::optional<std::string> file(const Card&, const std::string& /*rel*/) { return std::nullopt; }
};

class DiskSkills : public SkillSource {
public:
    explicit DiskSkills(std::vector<std::filesystem::path> roots) : roots_(std::move(roots)) {}
    std::string name() const override { return "disk"; }
    std::vector<Card> list() override;
    std::optional<std::string> body(const Card& c) override;
    std::optional<std::string> file(const Card& c, const std::string& rel) override;

private:
    struct Place {
        std::filesystem::path md, dir;
    };
    using Places = std::map<std::string, Place>;
    std::vector<std::filesystem::path> roots_;
    core::SnapCell<Places> places_;
};

class MazeSkills : public SkillSource {
public:
    explicit MazeSkills(std::shared_ptr<context::Memory> mem) : mem_(std::move(mem)) {}
    std::string name() const override { return "maze"; }
    std::vector<Card> list() override;
    bool ok() const override { return ok_.load(); }
    size_t skipped() const override { return skipped_.load(); }
    void cancel(bool on) override { cancel_.store(on); }
    std::optional<std::string> body(const Card& c) override;

private:
    std::shared_ptr<context::Memory> mem_;
    std::atomic<bool> cancel_{false};
    using Paths = std::map<std::string, std::filesystem::path>;   // "<source>/<name>" -> the SKILL.md a pointer card names
    std::atomic<bool> ok_{false};
    std::atomic<size_t> skipped_{0};
    core::SnapCell<Paths> paths_;
};

class SkillCatalogue : public Catalogue {
public:
    struct Options {
        size_t page_bytes = 5000;                  // one page of a skill: ~1.4k tokens
        std::set<std::string> disabled;            // names to hide
        std::map<std::string, double> priors;      // name -> weight on the final score (a hub below 1)
        Bm25fParams bm25;                          // field weights
    };
    // sources in precedence order
    explicit SkillCatalogue(std::vector<std::shared_ptr<SkillSource>> sources) : SkillCatalogue(std::move(sources), Options{}) {}
    SkillCatalogue(std::vector<std::shared_ptr<SkillSource>> sources, Options o);

    Kind kind() const override { return Kind::Skill; }
    bool ready() const override { return !snap_.empty(); }
    bool wait_ready(std::chrono::milliseconds t) const override { return snap_.wait(t); }
    bool healthy() const override { return healthy_.load(); }
    uint64_t generation() const override;
    size_t size() const override;
    std::vector<Hit> search(const Query& q, int k) const override;
    std::optional<Body> open(const Uri& u) override;
    std::string map() const override;
    void refresh() override;
    void cancel(bool on) override {
        for (auto& s : sources_) s->cancel(on);
    }
    float idf(const std::string& term) const override;

    std::optional<Card> find(const std::string& name) const;   // by uri id
    std::vector<Card> cards() const;
    struct Census {
        size_t cards = 0, bodyless = 0, collapsed = 0, superseded = 0, aliased = 0, skipped = 0;
        std::map<std::string, size_t> by_group, by_source;
    };
    Census census() const;

private:
    struct Owned {
        std::shared_ptr<SkillSource> src;
        Card orig;   // the card as its source made it: the source opens by that
    };
    struct Snapshot {
        std::unique_ptr<Bm25f> index;
        std::vector<Owned> owned;                         // parallel to index->cards()
        std::map<std::string, size_t> by_id;
        Census census;
        uint64_t generation = 0;
        uint64_t fingerprint = 0;
    };
    std::vector<std::shared_ptr<SkillSource>> sources_;
    Options opt_;
    core::SnapCell<Snapshot> snap_;
    std::atomic<bool> healthy_{false};
    std::atomic<uint64_t> gen_{0};
    std::vector<std::vector<Card>> last_good_;   // per source, what it last gave (the refresh thread's alone)
};

}  // namespace know
}  // namespace dd
