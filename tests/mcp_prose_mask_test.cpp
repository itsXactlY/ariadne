// tests/mcp_prose_mask_test.cpp — W5: a server's own prose is masked where it
// enters the prompt, and its schema is not.
//
// The proof is end-to-end, not a unit on the masker: a fake MCP server hands a
// description and instructions that carry a key-shaped token, a fake endpoint
// records the exact bytes dd_tui sends, and the assertions run on those bytes.
// Nothing is asserted about what the masker "should" do — the request body is the
// thing that ships, so the request body is what is checked. No tokens are spent:
// the fake endpoint answers with a canned stream.
//
// The box itself lives in tests/fake_mcp_box.hpp, shared with the boot tests: one
// fixture, so a change to the RPC shape is one change, not two that drift.
#include "dd/agent/redact.hpp"
#include "fake_mcp_box.hpp"

#include <cstdio>
#include <mutex>
#include <string>

namespace {

int g_fail = 0;
void chk(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

using fake::FakeBox;

// The masking rule, asserted directly: the two policies differ exactly where a
// slash-bearing token sits, which is the difference between a credential and a path.
void probe_rule() {
    using namespace dd::agent;
    const std::string key = FakeBox::kKey;
    chk(mask_secret_tokens("Answers a question. Key " + key + " is set.") ==
            "Answers a question. Key " + std::string(FakeBox::kKeyMasked) + " is set.",
        "a key-shaped token is masked");
    chk(mask_secret_tokens(FakeBox::kPath, SecretPolicy::ServerProse) == FakeBox::kPath,
        "a slash path in server prose stays byte-identical");
    chk(mask_secret_tokens(FakeBox::kEnum, SecretPolicy::ServerProse) == FakeBox::kEnum,
        "an enum value in server prose stays byte-identical");
    chk(mask_secret_tokens(FakeBox::kPath, SecretPolicy::ToolOutput) != FakeBox::kPath,
        "the same path in tool output is masked (the shipped rule)");
    chk(mask_secret_tokens("short 12345678 token and /plain/path", SecretPolicy::ToolOutput) ==
            "short 12345678 token and /plain/path",
        "short prose and short paths stay");
    // Whitespace survives byte for byte, so masking cannot reflow a description.
    const std::string nl = "line one\n" + key + "\nline three\n";
    const std::string m = mask_secret_tokens(nl, SecretPolicy::ServerProse);
    chk(m == "line one\n" + std::string(FakeBox::kKeyMasked) + "\nline three\n", "newline placement survives");
}

}  // namespace

int main() {
    probe_rule();
    if (g_fail) return 1;

    // ── end-to-end: what dd_tui actually sends ──
    FakeBox box;
    box.start();
    if (box.port == 0) return 1;

    const std::string home = "/tmp/dd_prose_mask_home";
    ::system("rm -rf /tmp/dd_prose_mask_home && mkdir -p /tmp/dd_prose_mask_home");
    const std::string base = "http://127.0.0.1:" + std::to_string(box.port) + "/v1";
    // tool_search off: the deck defers anything past its threshold behind the search bridges,
    // and a deferred tool never rides the prompt, so its description could not be observed there.
    const std::string cfg = "model:\n  base_url: " + base + "\n  name: fake-model\n"
                            "tool_search:\n  enabled: false\n"
                            "mcp_servers:\n  fake:\n    enabled: true\n    transport: http\n"
                            "    url: http://127.0.0.1:" + std::to_string(box.port) + "/mcp\n"
                            "    instructions: verbatim\n";
    {
        int f = ::open((home + "/config.yaml").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (f < 0) { std::printf("FAIL cannot write the temp home\n"); ++g_fail; box.halt(); return 1; }
        ::write(f, cfg.data(), cfg.size());
        ::close(f);
    }

    const std::string out_path = "/tmp/dd_prose_mask.out";
    int status = -1;
    fake::run_dd_tui(home, base, out_path, &status);
    box.halt();

    std::string body;
    {
        std::lock_guard<std::mutex> lk(box.mu);
        if (box.chat_bodies.empty()) {
            std::printf("FAIL dd_tui never reached the endpoint (exit %d)\n", status);
            std::printf("paths the box saw:");
            for (const auto& s : box.paths_seen) std::printf(" %s", s.c_str());
            std::printf("\n");
            std::printf("--- dd_tui output ---\n%s\n", fake::slurp(out_path).substr(0, 2000).c_str());
            ++g_fail;
            return 1;
        }
        body = box.chat_bodies.front();
        {   // the body is the datum; write it so a failure is inspectable, not guessed at
            int f = ::open("/tmp/dd_prose_mask_body.json", O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (f >= 0) { ::write(f, body.data(), body.size()); ::close(f); }
            std::printf("chat bodies captured: %zu\n", box.chat_bodies.size());
        }
    }

    // The wire list is the datum; these dumps make a failure nameable rather than guessed at.
    {
        int f = ::open("/tmp/dd_prose_mask_mcp.log", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (f >= 0) { for (const auto& s : box.mcp_log) ::write(f, s.data(), s.size()); ::close(f); }
    }
    chk(body.find(FakeBox::kKey) == std::string::npos, "the server's key never rides the prompt");
    chk(body.find(FakeBox::kKeyMasked) != std::string::npos, "the masked description rides the prompt");
    chk(body.find(FakeBox::kInstrKey) == std::string::npos, "the server's instructions key never rides");
    chk(body.find(FakeBox::kInstrMasked) != std::string::npos, "the masked instructions ride the system prompt");
    chk(body.find("\"fake_tool\"") != std::string::npos, "the tool name stays byte-identical");
    chk(body.find(FakeBox::kEnum) != std::string::npos, "the enum value stays byte-identical");
    chk(body.find(FakeBox::kPath) != std::string::npos, "the path in the description stays byte-identical");
    chk(body.find("\"required\":[\"q\"]") != std::string::npos, "the schema node is deep-copied untouched");

    if (g_fail == 0) std::printf("server-prose masking: all ok\n");
    return g_fail ? 1 : 0;
}
