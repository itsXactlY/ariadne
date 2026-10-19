// tools/debug_helpers.py -> dd::tools
#ifndef DD_TOOLS_DEBUG_HELPERS_HPP
#define DD_TOOLS_DEBUG_HELPERS_HPP
#include <string>
#include <vector>
#include <filesystem>
#include "dd/core/json.hpp"

namespace dd {
namespace tools {

class DebugSession {
public:
    DebugSession(const std::string& tool_name, const std::string& env_var);
    ~DebugSession() = default;

    bool active() const;
    void log_call(const std::string& call_name, const core::Json& call_data);
    void save();
    core::Json get_session_info() const;

private:
    std::string tool_name_;
    std::string env_var_;
    std::string session_id_;
    std::string start_time_;
    std::filesystem::path log_dir_;
    bool enabled_ = false;
    std::vector<core::Json> calls_;
};

}  // tools
}  // dd
#endif
