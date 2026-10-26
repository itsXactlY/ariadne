// bench/sink-handback-probe.cpp — feed a newer draft to the real sink against a skills root that
// holds a file the model edited through edit_file. The sink must hand it back, not overwrite it.
//
//   build/dd_sink_handback_probe /tmp/dd-prove-home/skills learned edit-proof
//
// It prints the verdict and whether the file moved, so the proof is the pair:
//   verdict starts with "edited_by:" and accepted == false, and the bytes are unchanged.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "dd/learn/skill_sink.hpp"

namespace fs = std::filesystem;
using namespace dd;
using namespace dd::learn;

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: dd_sink_handback_probe <skills-root> <category> <name>\n";
        return 2;
    }
    const fs::path root = argv[1];
    Draft d;
    d.name = argv[3];
    d.category = argv[2];
    d.kind = "fixed";
    d.gist = "a draft written from the transcript, newer than the file on disk";
    d.body = "# Steps\n\n1. a draft body that would overwrite the model's work\n";
    d.digest = "aaaaaaaaaaaaaaaa";   // differs from the digest on disk, so it is not "unchanged"
    d.episode = "bbbbbbbbbbbbbbbb";
    d.tags = {"patch", "edit"};

    SkillSink sink(root, [] { return int64_t(1790000000); });
    const fs::path f = sink.path_of(d);
    std::string before;
    {
        std::ifstream in(f, std::ios::binary);
        before = std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const auto rec = sink.write(d, Verdict{true, {}});
    std::string after;
    {
        std::ifstream in(f, std::ios::binary);
        after = std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    std::printf("file:      %s\n", f.c_str());
    std::printf("accepted:  %s\n", rec.accepted ? "yes" : "no");
    std::printf("verdict:   %s\n", rec.verdict.c_str());
    std::printf("bytes:     %zu before, %zu after, %s\n", before.size(), after.size(),
                before == after ? "UNCHANGED" : "CHANGED");
    std::printf("edited_by present in the file after the write: %s\n",
                after.find("edited_by:") != std::string::npos ? "yes" : "no");
    return rec.accepted || before != after ? 1 : 0;   // accepted, or the file moved, is the failure
}
