// tools/skill_provenance.py -> dd::tools
#ifndef DD_TOOLS_SKILL_PROVENANCE_HPP
#define DD_TOOLS_SKILL_PROVENANCE_HPP
#include <string>

namespace dd {
namespace tools {

// ContextVar-equivalent write-origin binding (LIFO stack for reset tokens).
int set_current_write_origin(const std::string& origin);
void reset_current_write_origin(int token);
std::string get_current_write_origin();
bool is_background_review();

// RAII binding. The stack is per-thread: the learn loop writes on its own thread while the model's
// tools run on the agent's, and a shared stack would attribute one writer's write to the other.
struct WriteOriginScope {
    explicit WriteOriginScope(const std::string& origin) : token(set_current_write_origin(origin)) {}
    ~WriteOriginScope() { reset_current_write_origin(token); }
    WriteOriginScope(const WriteOriginScope&) = delete;
    WriteOriginScope& operator=(const WriteOriginScope&) = delete;
    int token;
};

}  // tools
}  // dd
#endif
