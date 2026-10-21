#pragma once
// dd::core::home — where config, skills, rules and state live.
//
// Beside the binary: the directory of the running executable is home unless
// told otherwise. The Python Daedalus's ~/.daedalus is never used implicitly;
// platform_default_daedalus_home() names it only for an explicit import.
//
// Resolution order (resolve_home, get_daedalus_home):
//   1. --home DIR                       (resolve_home only)
//   2. the directory of --config FILE   (resolve_home only)
//   3. context-local override           (set_daedalus_home_override; get_daedalus_home only)
//   4. DD_HOME env var
//   5. DAEDALUS_HOME env var            (the ported Python modules' name for it)
//   6. the directory of the executable
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

// The Python Daedalus's home (~/.daedalus on POSIX): only `dd import-daedalus` reads it.
std::filesystem::path platform_default_daedalus_home();

// Directory of the running executable, symlinks resolved; empty if unknown.
std::filesystem::path exe_dir();

// DD_HOME env, else DAEDALUS_HOME env, else the executable's directory.
std::filesystem::path daedalus_home_from_env();

// Full resolution: override -> env -> beside the binary.
std::filesystem::path get_daedalus_home();

struct Home {
    std::filesystem::path root;
    std::filesystem::path config() const { return root / "config.yaml"; }
    std::filesystem::path skills() const { return root / "skills"; }
    std::filesystem::path rules() const { return root / "rules"; }
    std::filesystem::path skins() const { return root / "skins"; }
    std::filesystem::path state() const { return root / "state"; }
    std::filesystem::path cache() const { return root / "cache"; }
    std::filesystem::path logs() const { return root / "logs"; }
};

// The home for this run. Absolute, lexically normal.
Home resolve_home(const std::string& home_flag = "", const std::string& config_flag = "");

// mkdir -p for the home and its working directories (not skills/rules/skins: they are the operator's).
bool ensure_home(const Home& h, std::string* why = nullptr);

// A config.yaml for a fresh home: a copy of `from` when given, else default_config().
// Never overwrites. Returns true when a file was written.
bool init_config(const Home& h, const std::string& from, std::string* why = nullptr);

// The reference config for a fresh home (examples/daedalus/config.yaml).
const char* default_config();

}  // namespace core
}  // namespace dd
