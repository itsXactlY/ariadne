#pragma once
// dd/context/recall.hpp — what the maze hands a turn, and the skills it holds.
//
// Recaller: one embedding recall per turn (the pod serialises them, ~7 s),
// then the graph around it in parallel, all cheap: spreading activation from
// the best hit read back by id, the facts extracted from the top hits, and —
// for a question about earlier — this session's own turns by id. Nothing the
// window already carries comes back (the ledger knows what is on the tape),
// and the block fits a budget. The intent classifier routes it (40 ms).
//
// SkillIndex: the skill catalogue lives in the maze (skill:<source>:<name>
// summaries, skillsrc:…#NNN parts). It is indexed once, in the background,
// and matched locally — a skill costs the turn no recall; skill_load reads
// the full text back by id.

#include <atomic>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "dd/context/memory.hpp"

namespace dd {
namespace context {

class SkillIndex {
public:
    struct Skill {
        std::string name, source, category, description;
        long id = 0;
    };
    explicit SkillIndex(std::shared_ptr<Memory> mem) : mem_(std::move(mem)) {}
    ~SkillIndex();
    SkillIndex(const SkillIndex&) = delete;
    SkillIndex& operator=(const SkillIndex&) = delete;

    void load_async();                       // reads the catalogue on its own thread
    bool ready() const { return data() != nullptr; }
    size_t size() const;
    // The best skill for this text, when it clears min_score (BM25).
    // 12: measured on the 503-skill catalogue, fitting skills score 14.5-35, unrelated text at most 7.8
    std::optional<Skill> match(const std::string& text, double min_score = 12.0) const;
    std::vector<std::pair<Skill, double>> search(const std::string& text, int limit) const;
    // The full SKILL.md, reassembled from its parts in label order; "" if unknown.
    std::string load(const std::string& name, std::string* resolved = nullptr) const;
    // For tests: index these rows (skill:… labels with their summaries).
    void load_rows(const std::vector<Hit>& rows);

private:
    struct Data {
        std::vector<Skill> skills;
        std::vector<std::pair<std::string, std::string>> docs;   // name -> searchable text
    };
    // Shared with the loader thread, which may outlive the index: it never
    // touches the index itself, only this slot.
    struct Slot {
        std::atomic<const Data*> data{nullptr};   // published once, read lock-free
        std::atomic<bool> alive{true};
        ~Slot() { delete data.load(); }
    };
    std::shared_ptr<Memory> mem_;
    std::shared_ptr<Slot> slot_ = std::make_shared<Slot>();
    static void publish(Slot& slot, const std::vector<Hit>& rows);
    const Data* data() const { return slot_->data.load(std::memory_order_acquire); }
};

struct Prefetch {
    std::string block;        // <memory-context>… or ""
    std::string intent, skill;
    int hits = 0, linked = 0, facts = 0, earlier = 0, dropped = 0;   // dropped: already in the window
    std::vector<long> ids;    // what was injected
    double ms = 0;
};

class Recaller {
public:
    Recaller(std::shared_ptr<Memory> mem, const SkillIndex* skills) : mem_(std::move(mem)), skills_(skills) {}
    // in_window: ids already on the tape; earlier: this session's turn ids that are not.
    Prefetch run(const std::string& query, const std::set<long>& in_window, const std::vector<long>& earlier,
                 size_t budget_chars = 6000) const;
    double skill_min = 12.0;   // BM25 score a skill needs to be offered

private:
    std::shared_ptr<Memory> mem_;
    const SkillIndex* skills_;
};

}  // namespace context
}  // namespace dd
