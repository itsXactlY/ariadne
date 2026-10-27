#pragma once
// Pure subset of agent/redact.py: the pure token masker. The regex cascade in
// redact_sensitive_text (PREFIX/ENV/JSON/AUTH/TELEGRAM/PRIVATE-KEY/DB/SIGNAL,
// incl. negative lookarounds) and the env-gated RedactingFormatter are NOT
// ported — lookarounds have no std::regex analogue here.
#include <cctype>
#include <string>

namespace dd {
namespace agent {

// _mask_token: <18 chars -> "***", else first6 + "..." + last4.
// Byte-faithful for ASCII tokens (Python truth is code-point based).
inline std::string mask_token(const std::string& token) {
    if (token.size() < 18) return "***";
    return token.substr(0, 6) + "..." + token.substr(token.size() - 4);
}

// Two policies over one rule, because the two kinds of text differ:
//   ToolOutput  — what a tool handed back. A long token carrying a '/' is masked
//                 (a credential can ride inside a path), which is the shipped
//                 behaviour for rendered results.
//   ServerProse — a server's own description/instructions. A slash alone is NOT a
//                 credential there: measured against the live servers, the slash
//                 branch alone would have altered 3 of 48 real descriptions
//                 (`benchmarks/targeted_rebake/rebake.py:`, `/peer/connections/pull.`,
//                 `pulse-pro/pulse-enterprise).`) — documentation the model needs,
//                 destroyed for a shape that is a path, not a key. So ServerProse
//                 masks a token only when it carries digits AND letters, which is
//                 what a hash or an api key actually looks like.
enum class SecretPolicy { ToolOutput, ServerProse };

// Mask every whitespace-delimited token that looks like a credential: >= 16 chars,
// with letters and digits (ServerProse), or letters+digits or a '/' (ToolOutput).
// Whitespace and every other byte are untouched, so masking cannot reflow a string.
inline std::string mask_secret_tokens(const std::string& s,
                                      SecretPolicy p = SecretPolicy::ToolOutput) {
    std::string out;
    const size_t n = s.size();
    for (size_t i = 0; i < n;) {
        if (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r') { out.push_back(s[i++]); continue; }
        size_t j = i;
        while (j < n && s[j] != ' ' && s[j] != '\t' && s[j] != '\n' && s[j] != '\r') ++j;
        const std::string tok = s.substr(i, j - i);
        bool digit = false, letter = false, slash = false;
        for (char c : tok) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (std::isdigit(u)) digit = true;
            else if (std::isalpha(u)) letter = true;
            else if (c == '/') slash = true;
        }
        const bool shaped = tok.size() >= 16 && ((digit && letter) || (p == SecretPolicy::ToolOutput && slash));
        out += shaped ? mask_token(tok) : tok;
        i = j;
    }
    return out;
}

}  // namespace agent
}  // namespace dd
