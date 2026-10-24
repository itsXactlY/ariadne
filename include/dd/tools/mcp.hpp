#pragma once
// dd/tools/mcp.hpp — Streamable-HTTP MCP client (JSON-RPC 2.0 over a single
// POST endpoint).
//
// Every request is a POST of a JSON-RPC 2.0 envelope to `url` with Accept:
// application/json, text/event-stream. A reply is read as SSE (`data:` events,
// multi-line, CRLF) or as plain JSON (one object, pretty-printed, or a batch);
// the object whose id matches the request is the answer. A server that issues
// an Mcp-Session-Id at `initialize` gets it echoed afterwards; one that does
// not (the mazemaker pod) is used statelessly, no handshake needed. http and
// https only, no redirects, a reply past max_bytes is an error.

#include <atomic>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "dd/core/json.hpp"
#include "dd/core/snap_cell.hpp"

namespace dd {
namespace tools {

// What one tools/call came back with. ok: the request got a JSON-RPC result; is_error: the tool itself reported
// isError; text: the concatenated content[*].text; error: why it did not work (transport, JSON-RPC error, tool error).
struct McpResult {
    bool ok = false;
    bool is_error = false;
    std::string text, error;
};

class MCPClient {
public:
    std::string url;              // e.g. http://127.0.0.1:8765/mcp
    long long timeout_ms = 60000; // per-request deadline
    size_t max_bytes = 8u << 20;  // a reply larger than this aborts the transfer
    // Extra request headers (an Authorization token); values go through expand_env at each request.
    std::vector<std::pair<std::string, std::string>> headers;

    // Send one JSON-RPC request; return the parsed "result" node, or Json::Null if the response carried none.
    // Throws on transport failure, on an HTTP error that carries no JSON-RPC answer, and (message included)
    // when the server answers with a JSON-RPC error.
    dd::core::Json rpc(const std::string& method,
                       const dd::core::Json& params) const;

    // initialize (protocol version, client info; the session id is kept, `notifications/initialized` follows)
    // -> the server's "instructions" string (the memory protocol), "" when absent.
    std::string instructions();

    // tools/list, every page -> the "tools" array of {name,description,inputSchema}
    // (Json::Arr), Json::Null on failure.
    dd::core::Json list_tools();

    // tools/call -> concatenated result.content[*].text. Any failure (transport, JSON-RPC error, isError)
    // comes back as {"error":"...","isError":true}, so callers that parse results see it as one.
    std::string call(const std::string& name, const dd::core::Json& args);
    // The same, with the outcome kept apart from the text; never throws.
    McpResult call_ex(const std::string& name, const dd::core::Json& args);

    // "Bearer ${TOKEN}" -> the value of $TOKEN ("" when unset); anything else is kept as written.
    static std::string expand_env(const std::string& s);

private:
    struct Outcome {
        bool got = false;      // the server answered this request
        bool failed = false;   // ... with a JSON-RPC error
        long long code = 0;
        std::string message;
        dd::core::Json result;
    };
    Outcome exchange(const std::string& method, const dd::core::Json& params, bool notification) const;

    mutable core::SnapCell<std::string> session_;   // one publisher: initialize, at boot
    mutable std::atomic<bool> initialized_{false};
};

}  // namespace tools
}  // namespace dd
