// tools/interrupt.py -> dd::tools
#ifndef DD_TOOLS_INTERRUPT_HPP
#define DD_TOOLS_INTERRUPT_HPP

#include <atomic>

namespace dd {
namespace tools {
namespace interrupt {

// The cancel hand-off, in one place. The flag itself belongs to whoever runs the call
// (in the harness: Agent::interrupt_requested); this module holds only the pointer to the
// flag that is *current*, so Esc and a signal stop the call that is running and cannot
// cancel the next one. That is why the module owns a pointer and not a bool: a global
// event would fire a turn that has not started yet.
//
// The pointer is one atomic word, so the signal handler may read it; nothing here takes a
// lock or allocates. Binding happens once per process, beside the Agent that owns the flag.
void bind(std::atomic<bool>* flag);   // the flag the current call watches; nullptr unbinds
std::atomic<bool>* current();         // the bound flag, or nullptr when nothing is running

// Flip the bound flag. Returns what it was, so a caller can say "interrupting…" exactly once.
// No-op when nothing is bound — Esc with no call running must not invent a cancelled turn.
bool signal();

void clear();                 // back to not interrupted; the next call starts clean
bool is_interrupted();        // read the bound flag; false when nothing is bound

}  // interrupt
}  // tools
}  // dd
#endif
