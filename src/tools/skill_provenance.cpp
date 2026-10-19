// tools/skill_provenance.py -> dd::tools
#include "dd/tools/skill_provenance.hpp"

#include <string>
#include <vector>

namespace dd {
namespace tools {

static std::string g_origin = "foreground";
static std::vector<std::string> g_stack;

int set_current_write_origin(const std::string& origin) {
    g_stack.push_back(g_origin);
    g_origin = origin.empty() ? std::string("foreground") : origin;
    return (int)g_stack.size();  // token = stack depth after push
}

void reset_current_write_origin(int token) {
    (void)token;
    if (!g_stack.empty()) {
        g_origin = g_stack.back();
        g_stack.pop_back();
    }
}

std::string get_current_write_origin() { return g_origin; }

bool is_background_review() { return g_origin == "background_review"; }

}  // tools
}  // dd
