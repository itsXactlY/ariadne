// src/tools/vision_tool.cpp — view_image: hand an image file to the wire as an image.
//
// The handler validates the file, then returns text and an embedded marker
// ([[IMAGE <sha256> <mime> <base64>]]) that the agent loop lifts into the
// tool message's images (core::Message::images) and strips from the text.
// message_to_json ships it as an OpenAI content part, so the model that
// speaks a vision dialect actually sees the pixels.
#include "dd/tools/vision_tool.hpp"
#include "dd/core/json.hpp"
#include "dd/tools/registry.hpp"
#include "dd/core/sha256.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

namespace dd {
namespace tools {

// ---------------------------------------------------------------------------
// base64: the alphabet the wire expects (OpenAI image_url form). A tiny table
// walk; no padding surprises in either direction.
static const char* b64_alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(std::string_view data) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    for (size_t i = 0; i < data.size(); i += 3) {
        const unsigned b0 = static_cast<unsigned char>(data[i]);
        const unsigned b1 = (i + 1 < data.size()) ? static_cast<unsigned char>(data[i + 1]) : 0u;
        const unsigned b2 = (i + 2 < data.size()) ? static_cast<unsigned char>(data[i + 2]) : 0u;
        const unsigned n = (b0 << 16) | (b1 << 8) | b2;
        out += b64_alphabet[(n >> 18) & 63u];
        out += b64_alphabet[(n >> 12) & 63u];
        out += (i + 1 < data.size()) ? b64_alphabet[(n >> 6) & 63u] : '=';
        out += (i + 2 < data.size()) ? b64_alphabet[n & 63u] : '=';
    }
    return out;
}

int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::string base64_decode(std::string_view b64) {
    std::string out;
    unsigned acc = 0;
    int bits = 0;
    for (const char c : b64) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        const int v = b64_val(c);
        if (v < 0) return out;  // garbage past a marker: stop with what decoded
        acc = (acc << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((acc >> bits) & 0xFFu);
        }
    }
    return out;
}

namespace {

constexpr long long kVisionMaxBytes = 8LL * 1024 * 1024;  // raw-file cap

std::string str_arg(const core::Json& args, const std::string& key) {
    const core::Json* v = core::json_get(args, key);
    if (!v) return "";
    if (v->type == core::Json::Str) return v->str;
    if (v->type == core::Json::Obj || v->type == core::Json::Arr)
        if (const core::Json* p = core::json_get(*v, "path"))
            if (p->type == core::Json::Str) return p->str;
    return "";
}

std::string b64_encode(const std::string& raw) {
    static const char* alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((raw.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= raw.size()) {
        const unsigned v = (static_cast<unsigned char>(raw[i]) << 16) |
                           (static_cast<unsigned char>(raw[i + 1]) << 8) |
                           static_cast<unsigned char>(raw[i + 2]);
        out += alpha[(v >> 18) & 63];
        out += alpha[(v >> 12) & 63];
        out += alpha[(v >> 6) & 63];
        out += alpha[v & 63];
        i += 3;
    }
    if (i + 1 == raw.size()) {
        const unsigned v = static_cast<unsigned char>(raw[i]) << 16;
        out += alpha[(v >> 18) & 63];
        out += alpha[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == raw.size()) {
        const unsigned v = (static_cast<unsigned char>(raw[i]) << 16) |
                           (static_cast<unsigned char>(raw[i + 1]) << 8);
        out += alpha[(v >> 18) & 63];
        out += alpha[(v >> 12) & 63];
        out += alpha[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

// The containers the wire is trusted to decode; sniffed from the head, not
// the extension (a .png that holds a jpeg is still a jpeg — and it is what
// the decoder gets).
std::string sniff_mime(const std::string& raw) {
    if (raw.size() >= 3 &&
        static_cast<unsigned char>(raw[0]) == 0xFF &&
        static_cast<unsigned char>(raw[1]) == 0xD8 &&
        static_cast<unsigned char>(raw[2]) == 0xFF)
        return "image/jpeg";
    if (raw.size() >= 8 && raw.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0) return "image/png";
    if (raw.size() >= 6 && raw.compare(0, 6, "GIF87a") == 0) return "image/gif";
    if (raw.size() >= 6 && raw.compare(0, 6, "GIF89a") == 0) return "image/gif";
    if (raw.size() >= 12 && raw.compare(0, 4, "RIFF") == 0 && raw.compare(8, 4, "WEBP") == 0)
        return "image/webp";
    return "";
}

}  // namespace anon

core::Json vision_tool_schema() {
    return core::json_parse(
        R"JSON({"type":"object","properties":{"path":{"type":"string","description":"absolute path of the image file to see (JPEG/PNG/GIF/WebP, up to 8 MiB)"}},"required":["path"]})JSON");
}

std::function<std::string(const core::Json&)> make_vision_tool() {
    return [](const core::Json& args) -> std::string {
        const std::string path = str_arg(args, "path");
        if (path.empty()) return tool_error("view_image requires 'path'.");

        std::ifstream in(path, std::ios::binary);
        if (!in) return tool_error("view_image: cannot open '" + path + "'.");
        const std::string raw((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        in.close();
        if (raw.empty()) return tool_error("view_image: '" + path + "' is empty.");
        if (static_cast<long long>(raw.size()) > kVisionMaxBytes)
            return tool_error("view_image: '" + path + "' is " + std::to_string(raw.size()) +
                              " bytes; the wire caps one image at " + std::to_string(kVisionMaxBytes) +
                              " raw bytes.");

        const std::string mime = sniff_mime(raw);
        if (mime.empty())
            return tool_error("view_image: '" + path + "' is not a recognized image (JPEG/PNG/GIF/WebP).");

        const std::string digest = core::sha256_hex(raw);
        const std::string b64 = b64_encode(raw);

        // The text says the whole fact (a text-only model still reads it); the
        // marker underneath carries the payload the loop lifts into `images`.
        // Plain text first, marker last; the loop lifts the marker into `images`.
        return "Image at '" + path + "' (" + std::to_string(raw.size()) +
               " bytes, mime=" + mime + ", sha256=" + digest +
               ") is on the wire as an image; describe what it shows before answering.\n"
               "[[IMAGE " + digest + " " + mime + " " + b64 + "]]";
    };
}

}  // namespace tools
}  // namespace dd
