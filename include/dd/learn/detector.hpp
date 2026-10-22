#pragma once
// dd/learn/detector.hpp — which stretch of work is worth a skill. A Strategy: each detector reads the window
// and returns a score in (0,1] and the slice it found.

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dd/learn/episode.hpp"

namespace dd {
namespace learn {

struct Detection {
    std::string kind;        // fixed | long | pinned
    std::string signature;   // fixed: the error signature that stopped recurring; long: the topic key
    double score = 0;
    Episode slice;
    uint32_t end_turn = 0, end_round = 0;   // the last round of the slice: what the recorder drops
};

class Detector {
public:
    virtual ~Detector() = default;
    virtual const char* name() const = 0;
    virtual std::optional<Detection> detect(const Episode& window) const = 0;
};

// An error signature seen at least `min_seen` times, then a stretch of `quiet_rounds` rounds without it
// that holds a call that worked.
class FixedAfterFailure final : public Detector {
public:
    FixedAfterFailure(int min_seen = 2, int quiet_rounds = 3) : min_seen_(min_seen), quiet_(quiet_rounds) {}
    const char* name() const override { return "fixed"; }
    std::optional<Detection> detect(const Episode& window) const override;

private:
    int min_seen_, quiet_;
};

// At least `min_rounds` consecutive rounds on one topic that end without a failed call.
class LongSuccess final : public Detector {
public:
    explicit LongSuccess(int min_rounds = 8) : min_rounds_(min_rounds) {}
    const char* name() const override { return "long"; }
    std::optional<Detection> detect(const Episode& window) const override;

private:
    int min_rounds_;
};

// The user asked (`/learn`): the whole window, once. arm() may be called from any thread.
class Pinned final : public Detector {
public:
    const char* name() const override { return "pinned"; }
    void arm() { armed_.store(true); }
    std::optional<Detection> detect(const Episode& window) const override;

private:
    mutable std::atomic<bool> armed_{false};
};

// The detector with the best score, or nullopt.
std::optional<Detection> best_detection(const std::vector<std::unique_ptr<Detector>>& ds, const Episode& window);

}  // namespace learn
}  // namespace dd
