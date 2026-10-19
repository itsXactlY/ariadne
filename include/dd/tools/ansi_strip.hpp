// Port of tools/ansi_strip.py — ECMA-48 / ANSI escape stripping.
#include <string>
#ifndef DD_TOOLS_ANSI_STRIP_HPP
#define DD_TOOLS_ANSI_STRIP_HPP
namespace dd {
namespace tools {
// Remove ANSI/ECMA-48 escape sequences. Fast path returns input unchanged
// when no ESC (U+001B) or C1 (U+0080-U+009F) codepoint is present.
std::string strip_ansi(const std::string& text);
// Sanitize untrusted text for terminal display: strip escapes, normalize CR
// to LF, then remove bare control characters (preserving tab and newline).
std::string sanitize_display_text(const std::string& text);
}  // namespace tools
}  // namespace dd
#endif
