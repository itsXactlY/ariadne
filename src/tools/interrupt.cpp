// tools/interrupt.py -> dd::tools
#include "dd/tools/interrupt.hpp"

#include <atomic>

namespace dd {
namespace tools {

static std::atomic<bool> g_interrupt_event{false};

void set_interrupt(bool active) {
    g_interrupt_event.store(active);
}

bool is_interrupted() {
    return g_interrupt_event.load();
}

}  // tools
}  // dd
