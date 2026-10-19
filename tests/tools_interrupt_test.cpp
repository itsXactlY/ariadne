// Verifies dd::tools interrupt + skill_provenance.
#include "dd/tools/interrupt.hpp"
#include "dd/tools/skill_provenance.hpp"

#include <iostream>
#include <string>

using namespace dd;
using namespace dd::tools;

static int failures = 0;
#define CHECK(cond, msg)                                   \
    do {                                                   \
        if (!(cond)) {                                     \
            std::cerr << "FAIL: " << msg << "\n";          \
            ++failures;                                    \
        }                                                  \
    } while (0)

int main() {
    // --- interrupt ---
    CHECK(!is_interrupted(), "default not interrupted");
    set_interrupt(true);
    CHECK(is_interrupted(), "set -> interrupted");
    set_interrupt(false);
    CHECK(!is_interrupted(), "clear -> not interrupted");

    // --- skill_provenance ---
    CHECK(get_current_write_origin() == "foreground", "default foreground");
    CHECK(!is_background_review(), "default not background review");

    int tok = set_current_write_origin("background_review");
    CHECK(is_background_review(), "inside review fork");
    CHECK(get_current_write_origin() == "background_review", "origin readback");

    // nested set + LIFO reset
    int tok2 = set_current_write_origin("assistant_tool");
    CHECK(get_current_write_origin() == "assistant_tool", "nested origin");
    reset_current_write_origin(tok2);
    CHECK(get_current_write_origin() == "background_review", "restored to outer");
    reset_current_write_origin(tok);
    CHECK(get_current_write_origin() == "foreground", "restored to default");
    CHECK(!is_background_review(), "back to foreground");

    // empty origin coerces to foreground (origin or "foreground")
    int tok3 = set_current_write_origin("");
    CHECK(get_current_write_origin() == "foreground", "empty -> foreground");
    reset_current_write_origin(tok3);

    if (failures) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "all interrupt/provenance tests passed\n";
    return 0;
}
