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
    // --- interrupt: the flag belongs to the call, the module holds the hand-off ---
    CHECK(!interrupt::is_interrupted(), "nothing bound -> not interrupted");
    CHECK(interrupt::current() == nullptr, "nothing bound -> no flag to flip");

    std::atomic<bool> first{false}, second{false};
    interrupt::bind(&first);
    CHECK(interrupt::current() == &first, "the bound flag is the one handed back");
    CHECK(!interrupt::signal(), "the first signal reports the flag was not set");
    CHECK(interrupt::is_interrupted(), "signal -> interrupted");
    CHECK(first.load(), "signal wrote the caller's own flag, not a module bool");
    CHECK(interrupt::signal(), "a second signal reports it was already set");
    interrupt::clear();
    CHECK(!interrupt::is_interrupted(), "clear -> not interrupted");
    CHECK(!first.load(), "clear cleared the caller's flag");

    // The invariant the live path needs: Esc stops the call that is running and must not
    // cancel the next one. A global flag would fail exactly this check.
    interrupt::bind(&first);
    interrupt::signal();
    interrupt::bind(&second);
    CHECK(!interrupt::is_interrupted(), "a new call is not cancelled by the previous call's Esc");
    CHECK(first.load(), "the previous call keeps what its Esc wrote");
    interrupt::bind(nullptr);
    CHECK(!interrupt::signal(), "Esc with nothing running flips nothing");
    CHECK(!second.load(), "a signal with no bound flag flips nothing");

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
