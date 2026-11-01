// tools/interrupt.py -> dd::tools
#include "dd/tools/interrupt.hpp"

#include <atomic>

namespace dd {
namespace tools {
namespace interrupt {

// One atomic word: the flag the running call watches. Owned by the caller, borrowed here.
static std::atomic<std::atomic<bool>*> g_flag{nullptr};

void bind(std::atomic<bool>* flag) {
    g_flag.store(flag, std::memory_order_relaxed);
}

std::atomic<bool>* current() {
    return g_flag.load(std::memory_order_relaxed);
}

bool signal() {
    std::atomic<bool>* f = g_flag.load(std::memory_order_relaxed);
    if (!f) return false;
    return f->exchange(true, std::memory_order_acq_rel);
}

void clear() {
    if (std::atomic<bool>* f = g_flag.load(std::memory_order_relaxed)) f->store(false, std::memory_order_relaxed);
}

bool is_interrupted() {
    std::atomic<bool>* f = g_flag.load(std::memory_order_relaxed);
    return f && f->load(std::memory_order_acquire);
}

}  // interrupt
}  // tools
}  // dd
