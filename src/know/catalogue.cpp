// src/know/catalogue.cpp — impl of dd/know/catalogue.hpp.
#include "dd/know/catalogue.hpp"

#include "dd/core/park.hpp"

namespace dd {
namespace know {

void Refresher::start() {
    if (th_.joinable()) return;
    stop_.store(false);
    for (auto& c : cats_) c->cancel(false);
    th_ = std::thread([this] { run(); });
}

void Refresher::poke() {
    poke_.store(true, std::memory_order_release);
    wake_.fetch_add(1, std::memory_order_release);
    core::unpark(wake_);
}

void Refresher::stop() {
    stop_.store(true, std::memory_order_release);
    wake_.fetch_add(1, std::memory_order_release);
    core::unpark(wake_);
    if (!th_.joinable()) return;
    for (auto& c : cats_) c->cancel(true);   // a sync half done is abandoned, not waited for
    th_.join();
    for (auto& c : cats_) c->cancel(false);
}

void Refresher::run() {
    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    for (;;) {
        const uint32_t seen = wake_.load(std::memory_order_acquire);   // read before looking: a poke after this is never missed
        if (stop_.load(std::memory_order_acquire)) return;
        if (poke_.exchange(false) || clock::now() >= next) {
            bool healthy = true;
            for (auto& c : cats_) {
                if (stop_.load(std::memory_order_acquire)) return;
                try {
                    c->refresh();
                } catch (...) {
                }
                healthy = healthy && c->healthy();
            }
            next = clock::now() + (healthy ? period_ : retry_);
        }
        core::park(wake_, seen, next - clock::now());   // asleep in the kernel until the next refresh is due, a poke or a stop
    }
}

}  // namespace know
}  // namespace dd
