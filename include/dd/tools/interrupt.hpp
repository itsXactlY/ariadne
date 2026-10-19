// tools/interrupt.py -> dd::tools
#ifndef DD_TOOLS_INTERRUPT_HPP
#define DD_TOOLS_INTERRUPT_HPP
namespace dd {
namespace tools {

// Global interrupt signal (threading.Event equivalent).
void set_interrupt(bool active);
bool is_interrupted();

}  // tools
}  // dd
#endif
