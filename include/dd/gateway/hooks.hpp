#pragma once
// Pure subset of gateway/hooks.py.
// Ported: the event-handler *selection* rule used by HookRegistry.emit
// (exact event_type match, then the "<base>:*" prefix-wildcard list where
// base is the text before the first ':'). Handler ids are opaque tokens;
// selection returns them in emit()'s firing order.
// NOT ported (importlib / yaml / asyncio / filesystem-coupled): HookRegistry
// construction, _register_builtin_hooks, discover_and_load, emit dispatch,
// async handler invocation, loaded_hooks.
#include <string>
#include <vector>
#include <map>

namespace dd {
namespace gateway {

// emit()'s handler selection: exact(event_type) ++ (base+":*" when ':' present).
std::vector<std::string> select_handlers(
    const std::map<std::string, std::vector<std::string>>& handlers,
    const std::string& event_type);

}  // namespace gateway
}  // namespace dd
