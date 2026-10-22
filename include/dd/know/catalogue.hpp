#pragma once
// dd/know/catalogue.hpp — what a body of knowledge promises, and the thread that keeps it fresh.
//
// A catalogue holds cards (cheap, local, searched in about a millisecond, never
// blocking) and opens bodies on demand (which may block: a file, the pod). It
// publishes an immutable snapshot, so any thread reads it without a lock; a
// refresh builds the next one on the side and swaps it in.

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "dd/know/types.hpp"

namespace dd {
namespace know {

class Catalogue {
public:
    virtual ~Catalogue() = default;
    virtual Kind kind() const = 0;
    virtual bool ready() const = 0;                      // a snapshot has been published
    virtual bool healthy() const { return ready(); }     // the last refresh reached every source
    virtual uint64_t generation() const = 0;             // bumps when the card set changes
    virtual size_t size() const = 0;
    virtual std::vector<Hit> search(const Query& q, int k) const = 0;   // local, lock-free
    virtual std::optional<Body> open(const Uri& u) = 0;                 // may block; owns its deadline
    virtual std::string map() const = 0;                 // the awareness line: what is here, in groups
    virtual void refresh() = 0;                          // blocking: the owner picks the thread
    virtual float idf(const std::string&) const { return 0.0f; }   // for choosing a query's terms
};

// Refreshes catalogues in the background: every `period` while they are healthy,
// every `retry` while one is not (a pod that was down at launch is picked up when it returns).
class Refresher {
public:
    Refresher(std::vector<std::shared_ptr<Catalogue>> cats,
              std::chrono::milliseconds period = std::chrono::minutes(10),
              std::chrono::milliseconds retry = std::chrono::seconds(15))
        : cats_(std::move(cats)), period_(period), retry_(retry) {}
    ~Refresher() { stop(); }
    Refresher(const Refresher&) = delete;
    Refresher& operator=(const Refresher&) = delete;

    void start();
    void poke() { poke_.store(true); }   // refresh now
    void stop();

private:
    void run();
    std::vector<std::shared_ptr<Catalogue>> cats_;
    std::chrono::milliseconds period_, retry_;
    std::thread th_;
    std::atomic<bool> stop_{false}, poke_{false};
};

}  // namespace know
}  // namespace dd
