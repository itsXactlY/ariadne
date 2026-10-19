// tools/audio_container.py -> dd::tools
#include "dd/tools/audio_container.hpp"

#include <cctype>
#include <map>
#include <string>

namespace dd {
namespace tools {

static std::string lower4(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

static bool starts_with(const std::string& data, const std::string& prefix) {
    return data.size() >= prefix.size() && data.compare(0, prefix.size(), prefix) == 0;
}

std::optional<std::string> sniff_container(const std::string& data) {
    const std::size_t n = data.size();
    if (n >= 8 && data.compare(4, 4, "ftyp") == 0) {
        if (n >= 12) {
            std::string brand = lower4(data.substr(8, 4));
            if (brand == "m4a " || brand == "m4b ") return "m4a";
        }
        return "mp4";
    }
    if (starts_with(data, "OggS")) return "ogg";
    if (starts_with(data, "fLaC")) return "flac";
    if (n >= 12 && data.compare(0, 4, "RIFF") == 0 && data.compare(8, 4, "WAVE") == 0) return "wav";
    if (starts_with(data, "ID3")) return "mp3";
    if (n >= 2 && static_cast<unsigned char>(data[0]) == 0xFF &&
        (static_cast<unsigned char>(data[1]) & 0xE0) == 0xE0) {
        if ((static_cast<unsigned char>(data[1]) & 0xF6) == 0xF0) return "aac";
        return "mp3";
    }
    if (starts_with(data, std::string("\x1a\x45\xdf\xa3", 4))) return "webm";
    return std::nullopt;
}

std::string sniff_audio_ext(const std::string& data, const std::string& fallback_ext) {
    std::string fallback = (fallback_ext.size() >= 1 && fallback_ext[0] == '.')
                               ? fallback_ext
                               : "." + fallback_ext;
    std::optional<std::string> container = sniff_container(data);
    if (!container) return fallback;
    if (*container == "mp4") return ".m4a";
    static const std::map<std::string, std::string> CONTAINER_TO_EXT = {
        {"m4a", ".m4a"}, {"mp4", ".mp4"}, {"ogg", ".ogg"}, {"flac", ".flac"},
        {"wav", ".wav"}, {"mp3", ".mp3"}, {"aac", ".aac"}, {"webm", ".webm"}};
    return CONTAINER_TO_EXT.at(*container);
}

}  // namespace tools
}  // namespace dd
