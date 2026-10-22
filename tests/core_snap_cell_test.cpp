// dd::core::SnapCell — lock-free snapshots: what a read sees, when a snapshot is freed, and a stress run
// whose snapshots are poisoned on destruction, so a use after free shows even without a sanitizer.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "dd/core/snap_cell.hpp"

using namespace dd::core;

static int g_fail = 0;
static void chk(bool ok, const std::string& label) {
    if (!ok) {
        ++g_fail;
        std::printf("FAIL %s\n", label.c_str());
    }
}

static std::atomic<int> g_live{0};   // snapshots alive

struct Snap {
    explicit Snap(int v) : version(v) {
        for (auto& x : data) x = v;
        ++g_live;
    }
    ~Snap() {
        for (auto& x : data) x = -1;   // a use after free reads this
        --g_live;
    }
    bool whole() const {
        for (int x : data)
            if (x != version) return false;
        return true;
    }
    int version;
    int data[64];
};

static std::unique_ptr<const Snap> make(int v) { return std::make_unique<const Snap>(v); }

static void test_basic() {
    SnapCell<Snap> c;
    chk(c.empty(), "basic: empty at first");
    {
        auto r = c.read();
        chk(!r && r.get() == nullptr, "basic: an empty cell reads as null");
    }
    c.publish(make(1));
    chk(!c.empty() && g_live == 1, "basic: published");
    {
        auto r = c.read();
        chk(r && r->version == 1 && r->whole(), "basic: reads what was published");
    }
    c.publish(make(2));
    {
        auto r = c.read();
        chk(r && r->version == 2 && (*r).whole(), "basic: reads the latest");
    }
    chk(g_live == 1 && c.retired() == 0, "basic: no reader pinned: the old snapshot is freed at once");
}

static void test_pinned() {
    SnapCell<Snap> c;
    c.publish(make(1));
    {
        auto held = c.read();
        c.publish(make(2));
        c.publish(make(3));
        chk(held->version == 1 && held->whole(), "pinned: a read keeps its snapshot through two publishes");
        chk(c.retired() == 2 && g_live == 3, "pinned: the retired ones wait for the reader");
        auto fresh = c.read();
        chk(fresh->version == 3, "pinned: a new read sees the latest");
    }
    c.publish(make(4));
    chk(c.retired() == 0 && g_live == 1, "pinned: once it ends, the next publish frees them");
}

static void test_nested_and_moved() {
    SnapCell<Snap> c;
    c.publish(make(1));
    {
        auto a = c.read();
        auto b = c.read();   // nested on one thread: pins once
        auto m = std::move(a);
        chk(!a && m && b && m->version == 1, "nested: a moved read leaves the source empty");
        c.publish(make(2));
        chk(c.retired() == 1 && m->whole(), "nested: pinned while either read lives");
        m = SnapCell<Snap>::Read();   // releases m only
        c.publish(make(3));
        chk(b->version == 1 && b->whole() && c.retired() == 2, "nested: the outer pin holds until the last read ends");
    }
    c.publish(make(4));
    chk(c.retired() == 0 && g_live == 1, "nested: everything is freed afterwards");
}

static void test_destruction() {
    {
        SnapCell<Snap> c;
        c.publish(make(1));
        auto held = c.read();
        c.publish(make(2));
        held = SnapCell<Snap>::Read();
    }   // the cell goes with its current and its retired snapshots
    chk(g_live == 0, "destruction: the cell frees what it holds");
}

static void test_wait() {
    using Clock = std::chrono::steady_clock;
    auto ms_since = [](Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); };
    SnapCell<Snap> c;
    auto t0 = Clock::now();
    chk(!c.wait(std::chrono::milliseconds(50)) && ms_since(t0) >= 45, "wait: an empty cell times out (" + std::to_string(ms_since(t0)) + " ms)");

    double woke = -1;
    bool got = false;
    Clock::time_point published;
    std::atomic<bool> waiting{false};
    std::thread waiter([&] {
        waiting = true;
        got = c.wait(std::chrono::seconds(10));
        woke = ms_since(published);
    });
    while (!waiting) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));   // let it fall asleep
    published = Clock::now();
    c.publish(make(1));
    waiter.join();
    chk(got && woke >= 0 && woke < 25, "wait: the first publish wakes the sleeper at once (" + std::to_string(woke) + " ms)");

    t0 = Clock::now();
    chk(c.wait(std::chrono::seconds(10)) && ms_since(t0) < 25, "wait: a cell that already holds a snapshot returns at once");
}

static void test_stress() {
    SnapCell<Snap> c;
    c.publish(make(0));
    std::atomic<bool> stop{false};
    std::atomic<int> torn{0}, backwards{0};
    std::atomic<long> reads{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 6; ++t)
        readers.emplace_back([&] {
            int last = -1;
            while (!stop.load(std::memory_order_relaxed)) {
                auto r = c.read();
                if (!r) continue;
                if (!r->whole()) ++torn;
                if (r->version < last) ++backwards;
                last = r->version;
                ++reads;
            }
        });
    const int kVersions = 20000;
    for (int v = 1; v <= kVersions; ++v) {
        c.publish(make(v));
        if (v % 256 == 0) std::this_thread::yield();
    }
    stop = true;
    for (auto& th : readers) th.join();
    chk(torn == 0, "stress: no read saw a torn or freed snapshot (" + std::to_string(torn.load()) + ")");
    chk(backwards == 0, "stress: versions never go backwards on a thread (" + std::to_string(backwards.load()) + ")");
    chk(reads > 0, "stress: the readers ran");
    c.publish(make(kVersions + 1));   // the readers are gone: whatever was retired is freed now
    chk(c.retired() == 0 && g_live == 1, "stress: nothing retired is left behind (" + std::to_string(g_live.load()) + " alive)");
}

static void test_churn() {
    SnapCell<Snap> c;
    c.publish(make(0));
    std::atomic<bool> stop{false};
    std::atomic<int> torn{0};
    std::thread writer([&] {
        int v = 1;
        while (!stop.load()) {
            c.publish(make(v++));
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });
    const size_t before = snap::record_count();
    for (int i = 0; i < 300; ++i) {   // threads that read once and end
        std::thread t([&] {
            auto r = c.read();
            if (r && !r->whole()) ++torn;
        });
        t.join();
    }
    stop = true;
    writer.join();
    const size_t after = snap::record_count();
    chk(torn == 0, "churn: no torn read");
    chk(after <= before + 2, "churn: thread records are reused, not piled up (" + std::to_string(before) + " -> " + std::to_string(after) + ")");
}

int main() {
    test_basic();
    test_pinned();
    test_nested_and_moved();
    test_destruction();
    test_wait();
    test_stress();
    test_churn();
    chk(g_live == 0, "end: no snapshot outlives its cell (" + std::to_string(g_live.load()) + " alive)");
    if (g_fail) std::printf("%d FAILED\n", g_fail);
    else std::printf("all snap cell tests passed\n");
    return g_fail ? 1 : 0;
}
