// tests/core_wire_messages_test.cpp — the wire shape a chat template may insist on.
//
// The failure this pins down, measured 2026-11-06 against llama-server 8080 serving
// Qwen3.5-4B-GLB-3D-Coder: [system, user, system] → HTTP 500 "System message must be at the
// beginning." (tokenizer.chat_template line 83 raises for any system role that is not
// loop.first), while the same three messages with the last one role=user answered 200. Every
// emitter in the harness now writes that side channel as a hidden user item; this test is the
// rule the transport enforces for whatever path forgets.
#include "dd/core/message.hpp"

#include <iostream>
#include <string>
#include <vector>

using namespace dd::core;

static int g_fail = 0;
static void chk(const std::string& what, bool ok) {
    if (!ok) {
        std::cout << "FAIL " << what << "\n";
        ++g_fail;
    }
}

static Message m(const std::string& role, const std::string& content) {
    Message x;
    x.role = role;
    x.content = content;
    return x;
}

static std::string roles(const std::vector<Message>& msgs) {
    std::string r;
    for (const auto& x : msgs) { r += x.role; r += ','; }
    return r;
}

static bool valid(const std::vector<Message>& msgs) {
    for (size_t i = 0; i < msgs.size(); ++i) {
        if (msgs[i].role == "system" && i != 0) return false;
        if (msgs[i].role == "system" && i == 0) {
            for (size_t j = 1; j < msgs.size(); ++j)
                if (msgs[j].role == "system") return false;
        }
    }
    return true;
}

int main() {
    {   // the valid shape is left byte-identical: it is the prefix the server has cached
        std::vector<Message> v { m("system", "S"), m("user", "Q"), m("assistant", "A") };
        chk("valid:no-merge", collapse_system_messages(v) == 0);
        chk("valid:untouched", roles(v) == "system,user,assistant," && v[0].content == "S");
    }
    {   // the exact failing payload: the ledger rode as a second system message
        std::vector<Message> v { m("system", "S"), m("user", "Q"), m("system", "[ledger rev 1]") };
        chk("late-system:merged", collapse_system_messages(v) == 2);
        chk("late-system:shape", valid(v) && roles(v) == "system,user,");
        chk("late-system:folded-in-order", v[0].content == "S\n\n[ledger rev 1]");
    }
    {   // a system message that is the only one but not first still has to move
        std::vector<Message> v { m("user", "Q"), m("system", "S") };
        chk("out-of-place:merged", collapse_system_messages(v) == 1);
        chk("out-of-place:shape", valid(v) && roles(v) == "system,user,");
        chk("out-of-place:content", v[0].content == "S" && v[1].content == "Q");
    }
    {   // no system message at all: every body stays, and nothing is invented
        std::vector<Message> v { m("user", "Q"), m("assistant", "A") };
        chk("none:no-merge", collapse_system_messages(v) == 0);
        chk("none:untouched", roles(v) == "user,assistant,");
    }
    {   // empty content is not allowed to leave a stray separator behind
        std::vector<Message> v { m("system", "S"), m("user", "Q"), m("system", "") };
        chk("empty-late:merged", collapse_system_messages(v) == 2);
        chk("empty-late:content", v[0].content == "S");
    }
    {   // idempotent: the transport normalizes on every call, so a second pass must be a no-op
        std::vector<Message> v { m("system", "A"), m("user", "Q"), m("system", "B"), m("system", "C") };
        collapse_system_messages(v);
        const std::string once = v[0].content;
        chk("idempotent", collapse_system_messages(v) == 0 && v[0].content == once &&
                          once == "A\n\nB\n\nC");
    }
    if (g_fail == 0) std::cout << "ok: wire messages — one system message, at index 0\n";
    return g_fail == 0 ? 0 : 1;
}
