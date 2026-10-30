// tests/core_env_test.cpp — dd::core::env_present: set-to-nothing is not set.
//
// The product gates read env vars three ways: truthy string (`env_var_enabled`), a parsed value
// (`v && *v`), and presence. Presence is the one that drifts, because `DD_BASE_URL=` — a systemd
// unit's empty assignment, a wrapper forwarding unset vars, a shell that exported nothing — is
// present and empty, and a presence-only test reads it as a setting. That made a start with no
// config.yaml run on defaults from an unconfigured home instead of refusing it.
#include "dd/core/env.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

static int fails = 0;
static void check(bool cond, const std::string& what) {
    if (!cond) { std::cout << "FAILED: " << what << "\n"; ++fails; }
}

int main() {
    // Unset: not present.
    unsetenv("DD_PROBE_ENV");
    check(!dd::core::env_present("DD_PROBE_ENV"), "unset var reads as present");

    // Set to nothing: still not present. This is the case the product got wrong.
    setenv("DD_PROBE_ENV", "", 1);
    check(!dd::core::env_present("DD_PROBE_ENV"), "var set to \"\" reads as present");

    // Set to whitespace only: nothing someone wrote down is a value.
    setenv("DD_PROBE_ENV", "   ", 1);
    check(!dd::core::env_present("DD_PROBE_ENV"), "var set to spaces reads as present");
    setenv("DD_PROBE_ENV", "\t\n", 1);
    check(!dd::core::env_present("DD_PROBE_ENV"), "var set to a newline reads as present");

    // A real value, including ones that are not truthy words: presence is not truthiness.
    setenv("DD_PROBE_ENV", "http://127.0.0.1:8080/v1", 1);
    check(dd::core::env_present("DD_PROBE_ENV"), "var with a value reads as absent");
    setenv("DD_PROBE_ENV", "0", 1);
    check(dd::core::env_present("DD_PROBE_ENV"), "var set to \"0\" reads as absent");
    setenv("DD_PROBE_ENV", " ", 1);
    setenv("DD_PROBE_ENV", " x", 1);
    check(dd::core::env_present("DD_PROBE_ENV"), "var with a space then a value reads as absent");

    // The two conventions stay distinct: an empty value is neither present nor truthy, and a
    // truthy word is both. A gate that swapped them would change behaviour silently.
    setenv("DD_PROBE_ENV", "", 1);
    check(!dd::core::env_var_enabled("DD_PROBE_ENV"), "empty value reads as truthy");
    setenv("DD_PROBE_ENV", "true", 1);
    check(dd::core::env_var_enabled("DD_PROBE_ENV") && dd::core::env_present("DD_PROBE_ENV"),
          "a truthy word is not enabled and present together");

    unsetenv("DD_PROBE_ENV");
    std::cout << (fails ? "core_env_test: " + std::to_string(fails) + " failed\n"
                        : "core_env_test: ok\n");
    return fails ? 1 : 0;
}
