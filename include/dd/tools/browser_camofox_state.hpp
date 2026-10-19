#pragma once
// dd::tools::browser_camofox_state — Camofox profile-scoped identity + state paths.
// Port of tools/browser_camofox_state.py.
#include <filesystem>
#include <map>
#include <string>

namespace dd {
namespace tools {

// Profile-scoped root directory for Camofox persistence:
//   get_daedalus_home() / "browser_auth" / "camofox"
std::filesystem::path get_camofox_state_dir();

// Stable Daedalus-managed Camofox identity for this profile.
// Returns { "user_id", "session_key" }. task_id null -> "default" scope.
std::map<std::string, std::string> get_camofox_identity(const std::string* task_id);

}  // namespace tools
}  // namespace dd
