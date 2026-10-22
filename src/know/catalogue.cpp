// src/know/catalogue.cpp — impl of dd/know/catalogue.hpp.
#include "dd/know/catalogue.hpp"

namespace dd {
namespace know {

void Refresher::start() {
    if (th_.joinable()) return;
    stop_.store(false);
    th_ = std::thread([this] { run(); });
}

void Refresher::stop() {
    stop_.store(true);
    if (th_.joinable()) th_.join();
}

void Refresher::run() {
    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    while (!stop_.load()) {
        const bool poked = poke_.exchange(false);
        if (poked || clock::now() >= next) {
            bool healthy = true;
            for (auto& c : cats_) {
                if (stop_.load()) return;
                try {
                    c->refresh();
                } catch (...) {
                }
                healthy = healthy && c->healthy();
            }
            next = clock::now() + (healthy ? period_ : retry_);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

}  // namespace know
}  // namespace dd
