// tests/fake_shade.hpp — a dweller that answers instantly, for the level and the judge.
//
// Nothing here loads a model. What these tests are about is admission, routing, capacity and
// the front's answers; a fake shade makes those observable without a 23 GB load, and makes a
// failure (a dweller that dies mid-turn, one that refuses, one that cannot speak text) something
// a test can ask for on purpose rather than something it has to wait for a real engine to do.
#pragma once
#include <atomic>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

#include "dd/limbo/shade.hpp"

namespace dd {
namespace limbo {

class FakeShade : public Shade {
public:
    explicit FakeShade(ShadeSpec spec) : spec_(std::move(spec)) {}

    const ShadeSpec& spec() const override { return spec_; }
    bool speaks_text() const override { return speaks_text_.load(std::memory_order_acquire); }
    bool start(std::string& error) override {
        if (fail_start_.load(std::memory_order_acquire)) {
            error = "the fake dweller was told to fail its load";
            return false;
        }
        alive_.store(true, std::memory_order_release);
        starts_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    bool alive() const override { return alive_.load(std::memory_order_acquire); }
    Props props() const override {
        Props p;
        p.kind = spec_.kind;
        p.model = spec_.model_id;
        p.n_ctx = spec_.n_ctx > 0 ? spec_.n_ctx : 4096;
        p.slots = spec_.port > 0 ? 2 : 1;   // a shade named with a port stands for a parallel server
        p.alive = alive_.load(std::memory_order_acquire);
        core::obj_set(p.facts, "fake", core::json_bool(true));
        return p;
    }

    // What a turn does, chosen by the test:
    //   "reply"     — two pieces, a DONE-shaped finish, figures.
    //   "refuse"    — false with a reason; the dweller stays alive.
    //   "die"       — false, and alive() says false afterwards: a death at the process boundary.
    //   "slow"      — answers, but parks, so capacity is observable from another thread.
    // An atomic<int>, not an atomic<std::string>: libstdc++ has no lock-free specialization for a
    // string, and std::atomic<std::string> is a static_assert on trivially-copyable, not a type
    // this test may use. The names stay readable at the call site; the code is what races.
    enum Mode : int { Reply = 0, Refuse = 1, Die = 2, Slow = 3 };
    bool run(const TurnRequest& req, const DeltaSink& sink, TurnResult& out, std::string& error) override {
        turns_.fetch_add(1, std::memory_order_relaxed);
        last_prompt_.store(req.messages.size(), std::memory_order_release);
        const int mode = mode_.load(std::memory_order_acquire);
        if (mode == Refuse) {
            error = "the fake dweller refused this turn";
            return false;
        }
        if (mode == Die) {
            alive_.store(false, std::memory_order_release);
            error = "the fake dweller ended mid-turn";
            return false;
        }
        if (mode == Slow) {
            // A turn that holds its slot for a while: capacity becomes observable from another
            // thread instead of from a timing assumption.
            held_.store(true, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            held_.store(false, std::memory_order_release);
        }
        Delta d;
        d.content = spec_.name;
        if (!sink(d)) return true;   // the consumer went away; the turn is over, not failed
        Delta second;
        second.content = "-answer";
        if (!sink(second)) return true;
        out.finish = "stop";
        out.generated = 2;
        out.prompt_tokens = 10;
        out.reused = 4;
        out.prompt_ms = 3.0;
        out.decode_ms = 7.0;
        return true;
    }
    void interrupt() override { interrupted_.fetch_add(1, std::memory_order_relaxed); }
    void stop() override { alive_.store(false, std::memory_order_release); stops_.fetch_add(1, std::memory_order_relaxed); }

    void set_mode(const std::string& m) {
        mode_.store(m == "refuse" ? Refuse : m == "die" ? Die : m == "slow" ? Slow : Reply, std::memory_order_release);
    }
    void set_speaks_text(bool v) { speaks_text_.store(v, std::memory_order_release); }
    void set_fail_start(bool v) { fail_start_.store(v, std::memory_order_release); }
    int starts() const { return static_cast<int>(starts_.load(std::memory_order_relaxed)); }
    int stops() const { return static_cast<int>(stops_.load(std::memory_order_relaxed)); }
    int turns() const { return static_cast<int>(turns_.load(std::memory_order_relaxed)); }
    bool holding() const { return held_.load(std::memory_order_acquire); }
    int interrupted() const { return static_cast<int>(interrupted_.load(std::memory_order_relaxed)); }

private:
    ShadeSpec spec_;
    std::atomic<bool> alive_{false};
    std::atomic<bool> speaks_text_{true};
    std::atomic<bool> fail_start_{false};
    std::atomic<int> mode_{Reply};
    std::atomic<int> starts_{0};
    std::atomic<int> stops_{0};
    std::atomic<int> turns_{0};
    std::atomic<int> interrupted_{0};
    std::atomic<long> last_prompt_{0};
    std::atomic<bool> held_{false};
};

}  // namespace limbo
}  // namespace dd
