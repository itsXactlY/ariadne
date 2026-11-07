// tests/tool_vision_image_parts_test.cpp — ground truth for the native-vision path:
// view_image's payload rides the message as an image part, and an unterminated
// marker is never swallowed out of the tool's text.
#include "dd/tools/vision_tool.hpp"
#include "dd/core/message.hpp"
#include "dd/core/json.hpp"
#include "dd/core/sha256.hpp"
#include "dd/tools/registry.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

static int failures = 0;
#define CHECK(cond, label) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s\n", label); ++failures; } } while (0)

using namespace dd;

static std::string slurp(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return "";
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

int main(int argc, char** argv) {
    // A real image file may be passed (the runner passes one when it has one);
    // otherwise a PNG-headed body is written under DD_TEST_TMP. sniff_mime only
    // reads the container's head — the repo never decodes the pixels — so the
    // synthetic file exercises the same contract as a real one.
    std::string img = (argc > 1) ? argv[1] : "";
    std::string synth;
    const char* tmp = std::getenv("DD_TEST_TMP");
    if (img.empty() && tmp) {
        synth = std::string(tmp) + "/vision_sample.png";
        std::ofstream o(synth, std::ios::binary);
        const char magic[8] = {'\x89','P','N','G','\r','\n','\x1a','\n'};
        o.write(magic, 8);
        for (int i = 0; i < 64; ++i) o.put(static_cast<char>(i));
        o.close();
        img = synth;
    }

    // ---- the handler's own contract -------------------------------------
    const auto handler = tools::make_vision_tool();
    {
        core::Json a = core::json_obj();
        core::obj_set(a, "path", core::json_str(img));
        const std::string raw = slurp(img);
        const std::string res = handler(a);
        CHECK(!img.empty(), "sample file exists");
        CHECK(!raw.empty(), "sample is readable");
        CHECK(res.find("is on the wire as an image") != std::string::npos, "handler: fact sentence present");
        CHECK(res.find("sha256=") != std::string::npos, "handler: digest named in the text");
        CHECK(res.find("mime=image/png") != std::string::npos, "handler: sniffed mime is image/png");
        CHECK(res.find(core::sha256_hex(raw)) != std::string::npos, "handler: text digest equals sha256 of the bytes");

        // ---- the lift: the marker becomes an ImageRef, the text keeps the fact ----
        core::Message m;
        m.role = "tool";
        m.tool_call_id = "call_1";
        m.content = res;
        core::extract_tool_images(m);
        CHECK(m.images.size() == 1, "lift: exactly one image rides the message");
        if (m.images.size() == 1) {
            CHECK(m.images[0].mime == "image/png", "lift: mime survives");
            CHECK(m.images[0].digest == core::sha256_hex(raw), "lift: digest survives");
            CHECK(!m.images[0].b64.empty(), "lift: the payload survives");
        }
        CHECK(m.content.find("is on the wire as an image") != std::string::npos, "lift: the fact text is not eaten");
        CHECK(m.content.size() < res.size(), "lift: the marker was cut out of the text");

        // ---- the wire: message_to_json ships the OpenAI multimodal shape ----
        const std::string wire = core::json_dump(core::message_to_json(m));
        CHECK(wire.find("\"image_url\"") != std::string::npos, "wire: an image_url part is emitted");
        CHECK(wire.find("data:image/png;base64,") != std::string::npos, "wire: the payload is a png data URI");
        CHECK(wire.find("is on the wire as an image") != std::string::npos, "wire: the text part ships beside the image");
        CHECK(wire.find("\"role\":\"tool\"") != std::string::npos, "wire: role survives");

        // ---- the round trip: the same JSON comes home as the same message ----
        const core::Json parsed = core::json_parse(wire);
        const core::Message back = core::message_from_json(parsed);
        CHECK(back.images.size() == 1, "round trip: the image comes back");
        const bool same_payload = (back.images.size() == 1 && m.images.size() == 1 &&
                                   back.images[0].b64 == m.images[0].b64);
        const bool same_mime = (back.images.size() == 1 && m.images.size() == 1 &&
                                back.images[0].mime == m.images[0].mime);
        const bool digest_not_recomputed = (back.images.size() == 1 && back.images[0].digest.empty());
        CHECK(same_mime, "round trip: same mime");
        CHECK(digest_not_recomputed, "round trip: the digest is not recomputed on the way home");
        CHECK(back.content.find("sha256=") != std::string::npos, "round trip: the text still names the digest");
        CHECK(same_payload, "round trip: same payload");
        CHECK(back.content.find("is on the wire") != std::string::npos, "round trip: the text part returns to the text");
    }

    // ---- an unterminated marker is left alone: malformed tool text must
    // never swallow content on either side of it (the invariant the comment
    // in message.cpp:166 states). Feed the fact sentence plus a lone marker
    // byte; nothing may be cut, and no image may appear.
    if (!img.empty()) {
        core::Json a = core::json_obj();
        core::obj_set(a, "path", core::json_str(img));
        const std::string res = handler(a);
        std::string head_only = res;
        std::size_t ctrl = head_only.size();
        for (std::size_t i = 0; i < head_only.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(head_only[i]);
            if (c < 0x20) { ctrl = i; break; }
        }
        if (ctrl < res.size()) {
            core::Message m;
            m.role = "tool";
            m.content = std::string(res, 0, ctrl) + res[ctrl];   // marker opened, never closed
            core::extract_tool_images(m);
            CHECK(m.images.empty(), "unterminated marker: no image is invented");
            CHECK(m.content.size() >= std::string(res, 0, ctrl).size(), "unterminated marker: the text is not eaten");
        }
    }

    // ---- the refusals: a file that is not there, not an image, or too big ----
    {
        core::Json a = core::json_obj();
        core::obj_set(a, "path", core::json_str("/definitely/not/here.png"));
        CHECK(handler(a).find("cannot open") != std::string::npos, "refusal: missing file says cannot open");
        core::obj_set(a, "path", core::json_str(std::string()));
        CHECK(handler(a).find("requires 'path'") != std::string::npos, "refusal: empty path is refused");
        if (tmp) {
            const std::string notimg = std::string(tmp) + "/vision_notimage.bin";
            { std::ofstream o(notimg, std::ios::binary); o << "not an image in any container"; }
            core::Json b = core::json_obj();
            core::obj_set(b, "path", core::json_str(notimg));
            CHECK(handler(b).find("not a recognized image") != std::string::npos, "refusal: wrong magic is refused");
            const std::string big = std::string(tmp) + "/vision_toobig.bin";
            { std::ofstream o(big, std::ios::binary);
              o << "\x89PNG\r\n\x1a\n";
              for (long long i = 0; i < 9LL * 1024 * 1024; ++i) o.put('\0'); }
            core::Json c = core::json_obj();
            core::obj_set(c, "path", core::json_str(big));
            CHECK(handler(c).find("caps one image at") != std::string::npos, "refusal: the 8 MiB cap fires");
        }
    }

    // ---- the registration seam: the schema and the handler go in as one ToolEntry ----
    {
        tools::ToolRegistry reg;
        tools::ToolEntry e;
        e.name = "view_image";
        e.schema = tools::vision_tool_schema();
        e.handler = tools::make_vision_tool();
        reg.register_tool(e);
        const std::vector<std::string> names = reg.get_all_tool_names();
        bool found = false;
        for (const auto& n : names) found |= (n == "view_image");
        CHECK(found, "registration: view_image is in the registry the loop reads");
        const std::string s = core::json_dump(tools::vision_tool_schema());
        CHECK(s.find("\"path\"") != std::string::npos, "schema: takes a path");
        CHECK(s.find("required") != std::string::npos, "schema: path is required");
    }

    if (failures) std::fprintf(stderr, "%d vision-path checks failed\n", failures);
    else std::fprintf(stderr, "all vision-path tests passed\n");
    return failures ? 1 : 0;
}
