#pragma once
// Pure subset of gateway/channel_directory.py.
// Ported: _normalize_channel_query, _channel_target_name, _session_entry_id,
// _session_entry_name (state-independent dict -> string helpers).
// NOT ported (fs / adapter / sessions-coupled): build_channel_directory,
// _build_discord, _build_slack, _build_from_sessions, load_directory,
// resolve_channel_name, format_directory_for_display.
#include "dd/core/json.hpp"
#include <string>
#include <optional>

namespace dd {
namespace gateway {

// _normalize_channel_query: value.lstrip("#").strip().lower()
std::string normalize_channel_query(const std::string& value);

// _channel_target_name: discord+guild -> "#name"; non-discord+type -> "name (type)"
std::string channel_target_name(const std::string& platform_name, const core::Json& channel);

// _session_entry_id: None when chat_id falsy; "chat:thread" when thread truthy; else str(chat)
std::optional<std::string> session_entry_id(const core::Json& origin);

// _session_entry_name: base / topic-label composition from origin dict
std::string session_entry_name(const core::Json& origin);

}  // namespace gateway
}  // namespace dd
