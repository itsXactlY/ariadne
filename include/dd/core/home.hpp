#pragma once
// dd::core::home — Daedalus home resolution (port of daedalus_constants.py).
//
// Resolution order (matches get_daedalus_home()):
//   1. context-local override (set_daedalus_home_override)
//   2. DAEDALUS_HOME env var
//   3. platform-native default: home() / ".daedalus"
//
// The Python context-local override is a contextvar; here it is a process-global
// string (the harness runs one profile per process).

#include <filesystem>
#include <optional>
#include <string>

namespace dd {
namespace core {

// Set/clear the active context-local Daedalus home override.
void set_daedalus_home_override(const std::string& path);
std::optional<std::string> get_daedalus_home_override();

// Platform-native default home (~/.daedalus on POSIX).
std::filesystem::path platform_default_daedalus_home();

// DAEDALUS_HOME env, else platform default (ignores the override).
std::filesystem::path daedalus_home_from_env();

// Full resolution: override -> env -> default.
std::filesystem::path get_daedalus_home();

}  // namespace core
}  // namespace dd
