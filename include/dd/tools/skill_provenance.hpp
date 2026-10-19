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

}  // tools
}  // dd
#endif
