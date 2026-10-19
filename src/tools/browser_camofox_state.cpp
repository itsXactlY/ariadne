// dd::tools::browser_camofox_state — Camofox profile-scoped identity + state paths.
#include "dd/tools/browser_camofox_state.hpp"
#include "dd/core/home.hpp"
#include "dd/core/sha1.hpp"

#include <filesystem>
#include <map>
#include <string>

namespace dd {
namespace tools {

using namespace core;

static const std::string CAMOFOX_STATE_DIR_NAME = "browser_auth";
static const std::string CAMOFOX_STATE_SUBDIR = "camofox";

std::filesystem::path get_camofox_state_dir() {
    return get_daedalus_home() / CAMOFOX_STATE_DIR_NAME / CAMOFOX_STATE_SUBDIR;
}

std::map<std::string, std::string> get_camofox_identity(const std::string* task_id) {
    const std::string scope_root = get_camofox_state_dir().string();
    const std::string logical_scope = (task_id != nullptr && !task_id->empty()) ? *task_id : "default";

    const std::string user_digest = uuid5_url_hex("camofox-user:" + scope_root).substr(0, 10);
    const std::string session_digest =
        uuid5_url_hex("camofox-session:" + scope_root + ":" + logical_scope).substr(0, 16);

    std::map<std::string, std::string> out;
    out["user_id"] = "daedalus_" + user_digest;
    out["session_key"] = "task_" + session_digest;
    return out;
}

}  // namespace tools
}  // namespace dd
