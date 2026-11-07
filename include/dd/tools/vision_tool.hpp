#pragma once
// dd/tools/vision_tool.hpp — native vision: view_image hands an image file
// to the model as an actual image, not as a description of its bytes.
//
// The handler returns text plus an embedded image marker:
//   [[IMAGE <sha256-hex> <mime> <base64>]]
// The agent loop lifts the marker into the tool message's `images`
// (dd/core/message.hpp ImageRef) and strips it from the text; message_to_json
// puts the payload on the wire as an OpenAI content part
// ({"type":"image_url","image_url":{"url":"data:<mime>;base64,..."}}). The
// payload rides the message, not the text — so token accounting, the tape,
// and every text-only consumer never see the blob, and the text that stays
// states the same fact a multimodal model reads with its eyes. The digest is
// the file's sha256: the transcript can name an image it no longer carries,
// and a re-read of the same file is the same digest.

#include <functional>
#include <string>
#include "dd/core/json.hpp"

namespace dd {
namespace tools {

// The view_image schema, for register_tool at the call site.
core::Json vision_tool_schema();

// The view_image handler. No deps: the file is read here, so a harness that
// runs the tool under its own gate still gets a plain result.
std::function<std::string(const core::Json&)> make_vision_tool();

}  // namespace tools
}  // namespace dd
