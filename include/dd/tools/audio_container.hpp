// tools/audio_container.py -> dd::tools
#ifndef DD_TOOLS_AUDIO_CONTAINER_HPP
#define DD_TOOLS_AUDIO_CONTAINER_HPP
#include <optional>
#include <string>
namespace dd {
namespace tools {
// Container id from magic bytes, or nullopt when unknown (audio/AV only).
std::optional<std::string> sniff_container(const std::string& data);
// Container-matching extension, or fallback_ext when unknown.
std::string sniff_audio_ext(const std::string& data, const std::string& fallback_ext = ".ogg");
}  // namespace tools
}  // namespace dd
#endif
