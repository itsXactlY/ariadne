#pragma once
// Pure subset of gateway/mirror.py.
// Ported: _find_session_id's *matching/scoring* over an already-parsed
// sessions.json index object (selects origin.chat_id == chat_id on the
// lowercased platform, optional thread_id filter, newest updated_at wins;
// ties keep the first-seen entry). The index is supplied as data.
// NOT ported (fs / logging / sqlite-coupled): the sessions.json file read,
// mirror_to_session wrapper, _append_to_jsonl, _append_to_sqlite.
#include <string>
#include <optional>
#include "dd/core/json.hpp"

namespace dd {
namespace gateway {

// _find_session_id matching (given the parsed sessions.json object as `index`).
std::optional<std::string> find_session_id(
    const std::string& platform,
    const std::string& chat_id,
    const std::optional<std::string>& thread_id,
    const core::Json& index);

}  // namespace gateway
}  // namespace dd
