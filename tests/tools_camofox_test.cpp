// Tests for dd::tools Camofox state/identity helpers.
#include "dd/tools/browser_camofox_state.hpp"
#include "dd/core/home.hpp"

#include <iostream>
#include <map>
#include <string>

using namespace dd;

static int failures = 0;

static void check(const std::string& got, const std::string& want, const char* what) {
    if (got != want) {
        std::cerr << "FAIL " << what << ": got=" << got << " want=" << want << "\n";
        ++failures;
    }
}

int main() {
    // Pin the home so the digest is deterministic across machines.
    core::set_daedalus_home_override("/home/test/.daedalus");

    // State dir.
    check(tools::get_camofox_state_dir().string(), "/home/test/.daedalus/browser_auth/camofox", "state_dir");

    // Default scope (task_id null).
    {
        auto id = tools::get_camofox_identity(nullptr);
        check(id["user_id"], "daedalus_6239b11c43", "user_id(default)");
        check(id["session_key"], "task_5ca132bf9502541e", "session_key(default)");
    }

    // Explicit task scope.
    {
        const std::string tid = "task-42";
        auto id = tools::get_camofox_identity(&tid);
        check(id["user_id"], "daedalus_6239b11c43", "user_id(task-42)");
        check(id["session_key"], "task_f16f3257ec825ad0", "session_key(task-42)");
    }

    if (failures == 0) {
        std::cout << "all camofox tests passed\n";
        return 0;
    }
    std::cout << failures << " camofox test(s) failed\n";
    return 1;
}
