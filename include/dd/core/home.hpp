// dd::core::Home — where dd's own install lives: config and state, never Python's.
//
// The order, high to low — config and state live where the program was
// started, the install itself is where the binary sits:
//   1. --home
//   2. the folder of --config
//   3. DD_HOME / DAEDALUS_HOME
//   4. the binary's own folder, when it already holds a config.yaml (installed)
//   5. the folder dd was started from, when it holds a config.yaml (the project)
//   6. the binary's own folder, when it already holds state/ cache/ logs/ learn/
//   7. the folder dd was started from — where nothing of either kind exists yet
// Never picked, even by 6 or 7: a folder named build/ out/ obj/ is make's output
// and dd will not turn it into an install — an implicit home there is refused,
// with the reason. The install owns a config.yaml; the built binary is copied to
// it and run from there (`make install`).
// A folder that already carries its own config.yaml always governs, so an
// installed copy never leaks into the project it was called from and a folder
// dd was started from never writes into the install. Nothing here ever reads
// config.yaml or state/ from ~/.daedalus: that folder belongs to the Python
// install and sharing it makes the two fight over the files. Asking for it
// explicitly (--home ~/.daedalus, DD_HOME=~/.daedalus) is refused.

#ifndef DD_CORE_HOME_HPP
#define DD_CORE_HOME_HPP

#include <filesystem>
#include <optional>
#include <string>

namespace dd {
namespace core {

// Set/clear the active context-local Daedalus home override. A ported module
// that wants a folder handed to it calls this once, at startup.
void set_daedalus_home_override(const std::string& path);
std::optional<std::string> get_daedalus_home_override();

// $HOME/.daedalus — what the Python install uses. Kept only so `dd import-
// daedalus --from` can copy its skills and skins into a dd home, once.
std::filesystem::path platform_default_daedalus_home();

// True when p is that folder, whatever $HOME is: dd never lives there.
bool is_python_daedalus_home(const std::filesystem::path&);

// Directory of the running executable, symlinks resolved; empty if unknown.
std::filesystem::path exe_dir();

// DD_HOME, else DAEDALUS_HOME, else the folder that owns the config.yaml (the
// binary's first, then the launch folder), else a folder already in use, else
// the folder dd was started from. $HOME is never consulted.
std::filesystem::path daedalus_home_from_env();

// The home for this run: the override first, else the order above.
std::filesystem::path get_daedalus_home();

struct Home {
    std::filesystem::path root;  // the install: the folder holding config.yaml
    std::filesystem::path config() const { return root / "config.yaml"; }
    std::filesystem::path skills() const { return root / "skills"; }
    std::filesystem::path rules() const { return root / "rules"; }
    std::filesystem::path skins() const { return root / "skins"; }
    std::filesystem::path state() const { return root / "state"; }  // dd's own sqlite
    std::filesystem::path cache() const { return root / "cache"; }
    std::filesystem::path logs() const { return root / "logs"; }
    std::filesystem::path learn() const { return root / "learn"; }
};

// The home for this run. Absolute, lexically normal. When the result names the
// Python install's home — asked for by --home, or found in DD_HOME / DAEDALUS_HOME —
// the root is empty and *why (when non-null) says what to do instead: callers
// must check home.root before using it.
Home resolve_home(const std::string& home_flag = "", const std::string& config_flag = "",
                  std::string* why = nullptr);

// mkdir -p for the home and its working directories (not skills/rules/skins:
// they are the operator's). A plain start never calls this before it checked
// for config.yaml; --init is the only path that may create a home at all.
bool ensure_home(const Home& h, std::string* why = nullptr);

// A config.yaml for a fresh home: a copy of `from` when given, else default_config().
// Never overwrites. Returns true when a file was written.
bool init_config(const Home& h, const std::string& from, std::string* why = nullptr);

// The reference config for a fresh home (examples/daedalus/config.yaml).
const char* default_config();

}  // namespace core
}  // namespace dd
#endif  // DD_CORE_HOME_HPP
