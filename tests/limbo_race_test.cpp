// tests/limbo_race_test.cpp — the limbo level under ThreadSanitizer.
//
// Everything here is the same code the product runs; only the shade is fake and the timing is
// forced. The point is not "does it crash" but "what does a concurrent turn look like when it
// is refused": every request must end as exactly one answer — a completed completion, a refused
// status, or an error — and the counters must add up to what actually happened. A level that
// drops a turn silently passes a crash test and fails this one.
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "dd/limbo/level.hpp"
#include "dd/limbo/minos.hpp"
#include "fake_shade.hpp"

namespace dd {
namespace limbo {
namespace {

int g_fail = 0;
void chk(bool c, const std::string& what) {
    std::printf("%s   %s\n", c ? "ok" : "FAIL", what.c_str());
    if (!c) g_fail = 1;
}

// A Writer that records only the outcome, because the assertions are about outcomes: how many
// responses a turn produced and what the first one was. Nothing here blocks, so a stream is
// counted as one response and its body is kept only for the [DONE] question.
struct Recorder : public limbo::Writer {
    std::atomic<int> responses{0};
    std::atomic<int> code{0};
    std::atomic<bool> streaming{false};
    std::atomic<bool> open_{true};
    std::string first_body;

    bool respond(int status, std::string_view content_type, std::string_view body) override {
        (void)content_type;
        code.store(status, std::memory_order_relaxed);
        ++responses;
        if (first_body.empty()) first_body = std::string(body);
        return true;
    }
    bool stream_begin(int status, std::string_view content_type) override {
        (void)content_type;
        code.store(status, std::memory_order_relaxed);
        streaming.store(true, std::memory_order_relaxed);
        ++responses;
        return true;
    }
    bool stream_write(std::string_view bytes) override {
        if (!open_.load(std::memory_order_acquire)) return false;
        first_body += std::string(bytes);
        return true;
    }
    bool open() const override { return open_.load(std::memory_order_acquire); }

    int status() const { return code.load(std::memory_order_relaxed); }
    int responses_count() const { return responses.load(std::memory_order_relaxed); }
    bool is_streaming() const { return streaming.load(std::memory_order_relaxed); }
};

LimboConfig config() {
    LimboConfig c;
    ShadeSpec strata;
    strata.kind = ShadeKind::Strata;
    strata.name = "strata";
    strata.model_id = "moe";
    strata.n_ctx = 4096;
    ShadeSpec llama;
    llama.kind = ShadeKind::Llama;
    llama.name = "llama";
    llama.model_id = "dense";
    llama.port = 1;   // a parallel server: its ceiling is two turns, not one
    c.shades = {strata, llama};
    c.decoder = nullptr;   // the fake speaks text, so no lexicon is needed
    c.port = 0;
    c.build = [](const ShadeSpec& s) { return std::make_unique<FakeShade>(s); };
    return c;
}

std::string body(const char* model, bool stream) {
    return std::string("{\"model\":\"") + model + "\",\"stream\":" + (stream ? "true" : "false") +
           ",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
}

}  // namespace
}  // namespace limbo
}  // namespace dd

using namespace dd;

using namespace dd::limbo;   // chk and g_fail live in the anonymous namespace above

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // --- many turns on one dweller whose slot is held ---
    {
        limbo::LimboLevel level(limbo::config());
        std::string up;
        chk(level.start(up), "the level comes up: " + up);
        auto* strata = static_cast<limbo::FakeShade*>(level.minos().shade(0));
        strata->set_mode("slow");   // every strata turn holds its slot for a quarter second

        const int n_threads = 8;
        std::vector<std::thread> workers;
        std::vector<limbo::Recorder> rec(n_threads);
        std::atomic<int> completed{0}, refused{0}, broken{0};
        for (int i = 0; i < n_threads; ++i) {
            workers.emplace_back([&, i] {
                net::Request r;
                r.method = "POST";
                r.path = "/v1/chat/completions";
                r.body = limbo::body("moe", false);   // all of them want the held dweller
                level.route(r, rec[i]);
                const int s = rec[i].status();
                if (s == 200) completed.fetch_add(1, std::memory_order_relaxed);
                else if (s == 429 || s == 409 || s == 503) refused.fetch_add(1, std::memory_order_relaxed);
                else broken.fetch_add(1, std::memory_order_relaxed);
            });
        }
        // A status poller, at the same time: /slots is read while the counters are moving, which
        // is the only way its numbers are worth anything.
        std::atomic<int> polls{0};
        std::thread poller([&] {
            for (int i = 0; i < 40; ++i) {
                limbo::Recorder rec_slot;
                net::Request r;
                r.method = "GET";
                r.path = "/slots";
                level.route(r, rec_slot);
                if (rec_slot.status() == 200) polls.fetch_add(1, std::memory_order_relaxed);
                limbo::Recorder rec_props;
                r.method = "GET";
                r.path = "/props";
                level.route(r, rec_props);
            }
        });
        for (auto& t : workers) t.join();
        poller.join();

        chk(broken.load() == 0, "no turn ends as a status nobody expected");
        chk(completed.load() + refused.load() == n_threads, "every request answered exactly once");
        chk(refused.load() > 0, "a dweller whose slot is held refuses the excess, it does not queue it");
        chk(level.minos().slot(0).busy.load() == 0, "no turn is in flight once the workers are done");
        chk(level.minos().slot(0).turns.load() == completed.load(),
            "the dweller's turn count is the number of turns that actually ran");
        chk(polls.load() == 40, "the status paths answer while turns are moving");
        chk(level.minos().admitted() == 2, "a refused turn never costs a dweller its admission");
        level.stop();
    }

    // --- streams, interrupts, and a stop that lands mid-turn ---
    {
        limbo::LimboLevel level(limbo::config());
        std::string up;
        chk(level.start(up), "the level comes up: " + up);
        auto* llama = static_cast<limbo::FakeShade*>(level.minos().shade(1));
        llama->set_mode("slow");   // the parallel dweller, held

        std::vector<std::thread> workers;
        std::vector<limbo::Recorder> rec(6);
        std::atomic<int> streams{0}, refused{0};
        for (int i = 0; i < 6; ++i) {
            workers.emplace_back([&, i] {
                net::Request r;
                r.method = "POST";
                r.path = "/v1/chat/completions";
                r.body = limbo::body("dense", true);
                level.route(r, rec[i]);
                if (rec[i].is_streaming()) streams.fetch_add(1, std::memory_order_relaxed);
                else if (rec[i].status() == 429 || rec[i].status() == 503) refused.fetch_add(1, std::memory_order_relaxed);
            });
        }
        std::atomic<int> interrupts{0};
        std::thread interrupter([&] {
            for (int i = 0; i < 12; ++i) {
                level.minos().shade(1)->interrupt();
                interrupts.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
        std::thread stopper([&] {
            // Stop while turns are still in flight: the level must not hang on a pipe nobody is
            // reading any more, and no dweller may be left claiming it is busy.
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            level.stop();
        });
        for (auto& t : workers) t.join();
        interrupter.join();
        stopper.join();

        chk(streams.load() + refused.load() == 6, "every streamed request opened or refused, once");
        chk(interrupts.load() == 12, "an interrupt reaches the dweller with nothing in the way");
        chk(static_cast<FakeShade*>(level.minos().shade(1))->interrupted() >= 12, "the dweller saw every interrupt");
        chk(level.minos().admitted() == 0, "stop leaves nothing admitted, mid-turn or not");
        chk(level.minos().slot(1).busy.load() == 0, "no slot stays busy after a stop");
    }

    // --- two dwellers, one dying while the other answers ---
    {
        limbo::LimboLevel level(limbo::config());
        std::string up;
        chk(level.start(up), "the level comes up: " + up);
        static_cast<limbo::FakeShade*>(level.minos().shade(0))->set_mode("die");   // strata dies mid-turn

        std::atomic<int> moe_errors{0}, dense_ok{0};
        std::atomic<int> moe_not_ok{0};   // every non-200 answer to the dweller that is dying
        std::vector<std::thread> workers;
        for (int i = 0; i < 4; ++i) {
            workers.emplace_back([&, i] {
                limbo::Recorder rec;   // declared inside the thread: one per turn, no shared writer
                net::Request r;
                r.method = "POST";
                r.path = "/v1/chat/completions";
                r.body = limbo::body(i % 2 == 0 ? "moe" : "dense", false);
                level.route(r, rec);
                if (i % 2 == 0 && rec.status() >= 500) moe_errors.fetch_add(1, std::memory_order_relaxed);
                if (i % 2 == 1 && rec.status() == 200) dense_ok.fetch_add(1, std::memory_order_relaxed);
                // Anything but 200 is an honest answer to a dweller that is dying: an error, a
                // refusal for capacity, or a state the judge reported. 200 is the only lie.
                if (i % 2 == 0 && rec.status() != 200) moe_not_ok.fetch_add(1, std::memory_order_relaxed);
            });
        }
        for (auto& t : workers) t.join();

        chk(moe_not_ok.load() == 2,
            "a dying dweller answers with an error or a refusal — never a completion it did not produce");
        chk(moe_errors.load() >= 1, "at least one turn reached the dweller and saw it die");
        chk(dense_ok.load() == 2, "the dweller that survived keeps answering through the death");
        chk(level.minos().slot(0).respawns.load() >= 1, "the death is answered with a counted respawn");
        chk(level.minos().state_of(0) == limbo::ShadeState::ADMITTED, "the fallen dweller came back");
        level.stop();
    }

    std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "passed", g_fail);
    return g_fail ? 1 : 0;
}
