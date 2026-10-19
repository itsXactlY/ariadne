#pragma once
// Pure subset of agent/redact.py: the pure token masker. The regex cascade in
// redact_sensitive_text (PREFIX/ENV/JSON/AUTH/TELEGRAM/PRIVATE-KEY/DB/SIGNAL,
// incl. negative lookarounds) and the env-gated RedactingFormatter are NOT
// ported — lookarounds have no std::regex analogue here.
#include <string>

namespace dd {
namespace agent {

// _mask_token: <18 chars -> "***", else first6 + "..." + last4.
// Byte-faithful for ASCII tokens (Python truth is code-point based).
inline std::string mask_token(const std::string& token) {
    if (token.size() < 18) return "***";
    return token.substr(0, 6) + "..." + token.substr(token.size() - 4);
}

}  // namespace agent
}  // namespace dd
