// Pure subset of gateway/hooks.py (see header for the porting boundary).
#include "dd/gateway/hooks.hpp"
#include <string>
#include <vector>
#include <map>

namespace dd {
namespace gateway {

std::vector<std::string> select_handlers(
    const std::map<std::string, std::vector<std::string>>& handlers,
    const std::string& event_type) {
    std::vector<std::string> out;
    auto exact = handlers.find(event_type);
    if (exact != handlers.end())
        out.insert(out.end(), exact->second.begin(), exact->second.end());

    size_t colon = event_type.find(':');
    if (colon != std::string::npos) {
        std::string wildcard_key = event_type.substr(0, colon) + ":*";
        auto wild = handlers.find(wildcard_key);
        if (wild != handlers.end())
            out.insert(out.end(), wild->second.begin(), wild->second.end());
    }
    return out;
}

}  // namespace gateway
}  // namespace dd
