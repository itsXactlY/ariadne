// V4A patch parser — faithful port of parse_v4a_patch (tools/patch_parser.py).
#include "dd/tools/patch_parser.hpp"
#include <regex>
#include <cctype>
#include <string>
#include <vector>

namespace dd {
namespace tools {

static std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

static std::string strip(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

static bool contains(const std::string& s, const std::string& sub) {
    return s.find(sub) != std::string::npos;
}

std::pair<std::vector<PatchOperation>, std::optional<std::string>>
parse_v4a_patch(const std::string& patch_content) {
    std::vector<PatchOperation> operations;

    std::vector<std::string> lines = split_lines(patch_content);

    long start_idx = -1;  // Python: None -> -1 sentinel; else last Begin index
    bool have_start = false;
    long end_idx = -1;    // None sentinel
    for (long i = 0, n = (long)lines.size(); i < n; ++i) {
        const std::string& line = lines[i];
        if (contains(line, "*** Begin Patch") || contains(line, "***Begin Patch")) {
            start_idx = i;
            have_start = true;
        } else if (contains(line, "*** End Patch") || contains(line, "***End Patch")) {
            end_idx = i;
            break;
        }
    }
    if (!have_start) start_idx = -1;
    if (end_idx < 0) end_idx = (long)lines.size();

    static const std::regex re_update(R"(^\*\*\*\s*Update\s+File:\s*(.+))");
    static const std::regex re_add(R"(^\*\*\*\s*Add\s+File:\s*(.+))");
    static const std::regex re_delete(R"(^\*\*\*\s*Delete\s+File:\s*(.+))");
    static const std::regex re_move(R"(^\*\*\*\s*Move\s+File:\s*(.+?)\s*->\s*(.+))");
    static const std::regex re_hint(R"(^@@\s*(.+?)\s*@@)");

    long i = start_idx + 1;
    bool have_op = false;
    PatchOperation cur_op;
    bool have_hunk = false;
    Hunk cur_hunk;

    auto flush = [&]() {
        if (have_op) {
            if (have_hunk && !cur_hunk.lines.empty()) cur_op.hunks.push_back(cur_hunk);
            operations.push_back(cur_op);
        }
    };

    for (; i < end_idx; ++i) {
        const std::string& line = lines[i];
        std::smatch m;

        if (std::regex_search(line, m, re_update)) {
            flush();
            cur_op = PatchOperation{};
            cur_op.operation = OperationType::UPDATE;
            cur_op.file_path = strip(m[1].str());
            have_op = true;
            have_hunk = false;
        } else if (std::regex_search(line, m, re_add)) {
            flush();
            cur_op = PatchOperation{};
            cur_op.operation = OperationType::ADD;
            cur_op.file_path = strip(m[1].str());
            have_op = true;
            cur_hunk = Hunk{};
            have_hunk = true;
        } else if (std::regex_search(line, m, re_delete)) {
            flush();
            PatchOperation op;
            op.operation = OperationType::DELETE;
            op.file_path = strip(m[1].str());
            operations.push_back(op);
            have_op = false;
            have_hunk = false;
        } else if (std::regex_search(line, m, re_move)) {
            flush();
            PatchOperation op;
            op.operation = OperationType::MOVE;
            op.file_path = strip(m[1].str());
            op.new_path = strip(m[2].str());
            operations.push_back(op);
            have_op = false;
            have_hunk = false;
        } else if (line.rfind("@@", 0) == 0) {
            if (have_op) {
                if (have_hunk && !cur_hunk.lines.empty()) cur_op.hunks.push_back(cur_hunk);
                std::optional<std::string> hint;
                std::smatch hm;
                if (std::regex_search(line, hm, re_hint)) hint = hm[1].str();
                cur_hunk = Hunk{};
                cur_hunk.context_hint = hint;
                have_hunk = true;
            }
        } else if (have_op && !line.empty()) {
            if (!have_hunk) { cur_hunk = Hunk{}; have_hunk = true; }
            if (!line.empty() && line[0] == '+') {
                cur_hunk.lines.push_back(HunkLine{"+", line.substr(1)});
            } else if (line[0] == '-') {
                cur_hunk.lines.push_back(HunkLine{"-", line.substr(1)});
            } else if (line[0] == ' ') {
                cur_hunk.lines.push_back(HunkLine{" ", line.substr(1)});
            } else if (line[0] == '\\') {
                // pass
            } else {
                cur_hunk.lines.push_back(HunkLine{" ", line});
            }
        }
    }

    if (have_op) {
        if (have_hunk && !cur_hunk.lines.empty()) cur_op.hunks.push_back(cur_hunk);
        operations.push_back(cur_op);
    }

    return {operations, std::nullopt};
}

}  // tools
}  // dd
