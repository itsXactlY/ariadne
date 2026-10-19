// tests/tools_audio_container_test.cpp -> audio_container.py
#include "dd/tools/audio_container.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace dd::tools;
using namespace dd;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL: %s\n", #cond); } } while (0)

static std::string bytes(std::initializer_list<int> v) {
    std::string s;
    for (int b : v) s.push_back(static_cast<char>(b));
    return s;
}

int main() {
    // MP4 with audio brand -> m4a
    std::string m4a = bytes({0, 0, 0, 0x18, 'f', 't', 'y', 'p', 'M', '4', 'A', ' '});
    CHECK(sniff_container(m4a).value_or("") == "m4a");
    std::string m4b = bytes({0, 0, 0, 0x18, 'f', 't', 'y', 'p', 'm', '4', 'b', ' '});
    CHECK(sniff_container(m4b).value_or("") == "m4a");
    // generic mp4 brand -> mp4
    std::string mp4 = bytes({0, 0, 0, 0x18, 'f', 't', 'y', 'p', 'i', 's', 'o', 'm'});
    CHECK(sniff_container(mp4).value_or("") == "mp4");
    // ftyp but only 8 bytes -> mp4 (no brand)
    std::string shortmp4 = bytes({0, 0, 0, 0x18, 'f', 't', 'y', 'p'});
    CHECK(sniff_container(shortmp4).value_or("") == "mp4");

    CHECK(sniff_container("OggSxxxx").value_or("") == "ogg");
    CHECK(sniff_container("fLaCxxxx").value_or("") == "flac");

    std::string wav = bytes({'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'});
    CHECK(sniff_container(wav).value_or("") == "wav");
    // RIFF but WEBP form-type -> None (not audio)
    std::string webp = bytes({'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'});
    CHECK(!sniff_container(webp));

    CHECK(sniff_container("ID3\x04\x00").value_or("") == "mp3");

    // MP3 sync word 0xFF 0xFB
    std::string mp3sync = bytes({0xFF, 0xFB, 0x10, 0x00});
    CHECK(sniff_container(mp3sync).value_or("") == "mp3");
    // ADTS AAC 0xFF 0xF1
    std::string aac = bytes({0xFF, 0xF1, 0x14, 0x00});
    CHECK(sniff_container(aac).value_or("") == "aac");
    // 0xFF 0x00 -> not a sync word (0x00 & 0xE0 != 0xE0)
    std::string notsync = bytes({0xFF, 0x00, 0x10, 0x00});
    CHECK(!sniff_container(notsync));

    std::string webm = bytes({0x1a, 0x45, 0xdf, 0xa3, 0x00});
    CHECK(sniff_container(webm).value_or("") == "webm");

    CHECK(!sniff_container("hello world"));
    CHECK(!sniff_container(""));

    // ---- sniff_audio_ext ----
    CHECK(sniff_audio_ext(mp4) == ".m4a");  // mp4 -> .m4a in audio context
    CHECK(sniff_audio_ext(m4a) == ".m4a");
    CHECK(sniff_audio_ext("OggS") == ".ogg");
    CHECK(sniff_audio_ext(wav) == ".wav");
    CHECK(sniff_audio_ext("nope") == ".ogg");  // default fallback
    CHECK(sniff_audio_ext("nope", ".wav") == ".wav");
    CHECK(sniff_audio_ext("nope", "mp3") == ".mp3");  // adds leading dot
    CHECK(sniff_audio_ext("nope", "") == ".");       // "." + "" -> "."

    if (failures == 0) {
        std::printf("all audio-container tests passed\n");
        return 0;
    }
    std::printf("%d failures\n", failures);
    return 1;
}
