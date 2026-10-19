#pragma once
// dd/tools/mcp.hpp — Streamable-HTTP MCP client (JSON-RPC 2.0 over a single
// POST endpoint, SSE `text/event-stream` responses).
//
// The mazemaker pod speaks MCP-over-HTTP on a uvicorn endpoint: every request
// is a POST of a JSON-RPC 2.0 envelope to <base> with Accept:
// application/json, text/event-stream; every response is one or more SSE
// `data:` lines carrying a JSON-RPC result. The server issues no
// Mcp-Session-Id, so requests are stateless — no session management needed.

#include <string>
#include "dd/core/json.hpp"

namespace dd {
namespace tools {

class MCPClient {
public:
    std::string url;              // e.g. http://127.0.0.1:8765/mcp
    long long timeout_ms = 60000; // per-request deadline

    // Send one JSON-RPC request; return the parsed "result" node (Json::Obj),
    // or Json::Null if the response carried no result. Throws on transport
    // failure.
    dd::core::Json rpc(const std::string& method,
                       const dd::core::Json& params) const;

    // initialize -> the server's "instructions" string (the memory protocol),
    // "" when absent.
    std::string instructions();

    // tools/list -> the "tools" array of {name,description,inputSchema}
    // (Json::Arr), Json::Null on failure.
    dd::core::Json list_tools();

    // tools/call -> concatenated result.content[*].text ("" on failure).
    std::string call(const std::string& name, const dd::core::Json& args);
};

}  // namespace tools
}  // namespace dd
