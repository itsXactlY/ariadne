// dd::tools — the application half of the V4A patch format. patch_parser.cpp reads the format and
// hands back operations; this turns them into writes. See patch_apply.hpp for the contract: plan
// everything against the current files, then commit, so a patch with one bad hunk changes nothing.
#include "dd/tools/patch_apply.hpp"

#include "dd/tools/fuzzy_match.hpp"
#include "dd/tools/fs_tools.hpp"
#include "dd/tools/patch_parser.hpp"
#include "dd/tools/path_security.hpp"
#include "dd/tools/skill_provenance.hpp"

#include <ctime>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace dd {
namespace tools {

using namespace dd::core;
namespace fs = std::filesystem;

namespace {

JsonSP jsp(Json v) { return std::make_shared<Json>(std::move(v)); }
std::string jerr(const std::string& m) { return json_dump(json_obj({{"error", jsp(json_str(m))}})); }

const char* op_name(const PatchOperation& op) {
    switch (op.operation) {
        case OperationType::ADD: return "Add File";
        case OperationType::UPDATE: return "Update File";
        case OperationType::DELETE: return "Delete File";
        case OperationType::MOVE: return "Move File";
    }
    return "Patch operation";
}

// How the model wrote the operation, so an error points at the hunk it can rewrite.
std::string op_label(const PatchOperation& op) {
    std::string s = std::string(op_name(op)) + ": " + op.file_path;
    if (op.new_path) s += " -> " + *op.new_path;
    return s;
}

// One hunk -> the text to find and the text to put in its place. Context lines appear on both sides,
// '-' lines only on the old side, '+' lines only on the new side. Neither side gets a trailing
// newline: the matched span ends where the last hunk line ends, so whatever the file has after it
// (including its newline) survives.
struct HunkText {
    std::string old_s;
    std::string new_s;
    bool adds = false;
    bool removes = false;
};

HunkText hunk_text(const Hunk& h) {
    HunkText t;
    for (const auto& l : h.lines) {
        if (l.prefix == "-") {
            if (!t.old_s.empty()) t.old_s += "\n";
            t.old_s += l.content;
            t.removes = true;
        } else if (l.prefix == "+") {
            if (!t.new_s.empty()) t.new_s += "\n";
            t.new_s += l.content;
            t.adds = true;
        } else {
            if (!t.old_s.empty()) t.old_s += "\n";
            if (!t.new_s.empty()) t.new_s += "\n";
            t.old_s += l.content;
            t.new_s += l.content;
        }
    }
    return t;
}

// Applies one hunk to `content`, in place. Returns the failure naming the file and the hunk, or
// nullopt. The hint is echoed on failure only: fuzzy_match reports no match positions, so a hint
// cannot choose between candidates, but it tells the model which context to widen.
std::optional<std::string> apply_hunk(std::string& content, const Hunk& h, size_t idx,
                                      const std::string& file, std::vector<std::string>& notes) {
    const HunkText t = hunk_text(h);
    const std::string hint = h.context_hint ? " (hunk hint: " + *h.context_hint + ")" : "";
    const std::string where = file + " hunk " + std::to_string(idx + 1);

    if (!t.adds && !t.removes) {
        notes.push_back(where + ": context only, nothing to change");
        return std::nullopt;
    }
    if (!t.removes) {
        if (t.old_s.empty()) {  // nothing to anchor to: appended, and the note says so
            std::string add = t.new_s;
            if (!content.empty() && content.back() != '\n') add = "\n" + add;
            content += add + "\n";
            notes.push_back(where + ": appended at the end of the file (add-only hunk" +
                            (h.context_hint ? ", hint " + *h.context_hint + " was not used as a position" : "") + ")");
            return std::nullopt;
        }
        // Context plus additions with no removals: the context IS the position. Replacing the
        // matched region with new_s puts the additions where the hunk ordered them; falling back to
        // an append would dump them at EOF and duplicate the context lines with them. An unmatched
        // context fails here rather than silently landing somewhere else.
        const FuzzyResult r = fuzzy_find_and_replace(content, t.old_s, t.new_s, false);
        if (r.error) return where + ": " + *r.error + hint;
        notes.push_back(where + ": inserted at " + std::to_string(r.match_count) + " place(s)" + hint);
        content = r.content;
        return std::nullopt;
    }

    const FuzzyResult r = fuzzy_find_and_replace(content, t.old_s, t.new_s, false);
    if (r.error) return where + ": " + *r.error + hint;
    notes.push_back(where + ": matched " + std::to_string(r.match_count) + " place(s)" + hint);
    content = r.content;
    return std::nullopt;
}

// A learned skill the model edited records who edited it. The learning loop reads this to tell a file
// it authored from one the model touched, and leaves the model's edit alone. Body bytes are preserved
// exactly: only the frontmatter block is rebuilt.
std::string annotate_edited(const std::string& content, const std::string& origin, long long now) {
    if (content.rfind("---\n", 0) != 0) return content;
    std::vector<std::string> fm;
    size_t off = 4;
    size_t close = std::string::npos;
    while (off < content.size()) {
        const size_t nl = content.find('\n', off);
        std::string line = nl == std::string::npos ? content.substr(off) : content.substr(off, nl - off);
        if (line == "---") { close = off; break; }
        fm.push_back(std::move(line));
        if (nl == std::string::npos) break;
        off = nl + 1;
    }
    if (close == std::string::npos) return content;
    bool learned = false;
    for (const auto& l : fm)
        if (l.rfind("origin: learned", 0) == 0) learned = true;
    if (!learned) return content;

    std::string out = "---\n";
    for (const auto& l : fm)
        if (l.rfind("edited_by:", 0) != 0 && l.rfind("edited_at:", 0) != 0) out += l + "\n";
    out += "edited_by: " + (origin.empty() ? std::string("model_edit") : origin) + "\n";
    out += "edited_at: " + std::to_string(now) + "\n";
    out += content.substr(close);
    return out;
}

// Is this path a skill file the learn loop owns?
bool is_learned_skill(const std::string& path, const std::optional<std::string>& root) {
    if (!root) return false;
    if (fs::path(path).filename() != "SKILL.md") return false;
    const std::string p = fs::weakly_canonical(fs::path(path)).string();
    const std::string r = fs::weakly_canonical(fs::path(*root)).string();
    return r.size() < p.size() && p.rfind(r + "/", 0) == 0;
}

// The label for a committed file's op, for the error that names a partial commit.
std::string op_name_for(const std::string& op) { return op == "Add" ? "Add File" : op == "Move" ? "Move File" : "Update File"; }

std::vector<std::string> split_patch_lines(const std::string& text) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) { out.push_back(text.substr(pos)); break; }
        out.push_back(text.substr(pos, nl - pos));
        pos = nl + 1;
    }
    if (!out.empty() && out.back().back() == '\r') out.back().pop_back();
    return out;
}

// The format attaches hunks to Update and Add only: a Move line closes its operation, so hunk lines
// written after it are dropped by the parser. Reporting success would let the model believe its edit
// landed, so the patch is refused and told what to do instead.
std::optional<std::string> dropped_hunks(const std::string& patch_text) {
    bool in_patch = false;
    bool after_move = false;
    std::string moved;
    for (const std::string& line : split_patch_lines(patch_text)) {
        if (line.find("*** Begin Patch") != std::string::npos || line.find("***Begin Patch") != std::string::npos) {
            in_patch = true;
            continue;
        }
        if (line.find("*** End Patch") != std::string::npos || line.find("***End Patch") != std::string::npos) break;
        if (!in_patch) continue;
        if (line.rfind("***", 0) == 0) {
            const size_t at = line.find("Move File:");
            after_move = at != std::string::npos;
            if (after_move) {
                std::string rest = line.substr(at + 10);
                const size_t arrow = rest.find("->");
                moved = arrow == std::string::npos ? rest : rest.substr(0, arrow);
                while (!moved.empty() && moved.front() == ' ') moved.erase(0, 1);
                while (!moved.empty() && moved.back() == ' ') moved.pop_back();
            }
            continue;
        }
        if (after_move && (line.front() == '+' || line.front() == '-' || line.front() == ' ' || line.rfind("@@", 0) == 0))
            return "Move File: " + moved + " takes no hunks — the format attaches hunks to Update and Add only. "
                   "Move the file, then Update the new path.";
    }
    return std::nullopt;
}

struct Planned {
    std::unordered_map<std::string, std::string> staged;   // path -> the content it will be written
    std::vector<std::string> removes;                      // paths unlinked after the writes
    std::vector<AppliedFile> order;                        // commit order, as reported
};

std::optional<std::string> plan_one(const PatchOperation& op, Planned& pl, const ApplyOptions& opt,
                                    std::vector<std::string>& notes) {
    if (has_traversal_component(op.file_path))
        return op_label(op) + ": path contains '..': " + op.file_path;
    const std::string dest = op.new_path ? *op.new_path : op.file_path;
    if (op.operation == OperationType::MOVE) {
        if (has_traversal_component(dest))
            return op_label(op) + ": destination contains '..': " + dest;
        if (dest == op.file_path)
            return op_label(op) + ": source and destination are the same path";
    }
    if (!opt.root.empty() && (op.operation == OperationType::DELETE || op.operation == OperationType::MOVE)) {
        for (const std::string& p : {op.file_path, dest}) {
            if (const auto esc = validate_within_dir(p, opt.root)) return op_label(op) + ": " + *esc;
        }
    }
    if ((op.operation == OperationType::DELETE || op.operation == OperationType::MOVE) && !opt.allow_delete)
        return std::string(op_name(op)) + ": removing a file needs allow_delete=true (" + op.file_path + ")";

    std::error_code ec;
    std::string err;
    switch (op.operation) {
        case OperationType::ADD: {
            if (pl.staged.count(dest)) return "the patch writes " + dest + " twice";
            std::string body;
            for (const auto& h : op.hunks)
                for (const auto& l : h.lines)
                    if (l.prefix == "+" || l.prefix == " ") {
                        if (!body.empty()) body += "\n";
                        body += l.content;
                    }
            if (body.empty()) return "Add File: " + dest + " has no content";
            body += "\n";
            if (fs::exists(dest, ec)) return "Add File: " + dest + " already exists (use Update File)";
            pl.staged[dest] = body;
            pl.order.push_back({dest, "Add", body.size()});
            return std::nullopt;
        }
        case OperationType::UPDATE: {
            std::string content;
            if (pl.staged.count(dest)) content = pl.staged[dest];   // a second op on this file continues from the first
            else {
                content = fs_read_raw(dest, opt.max_bytes, err);
                if (!err.empty()) return "Update File: " + dest + ": " + err;
            }
            for (size_t i = 0; i < op.hunks.size(); ++i)
                if (const auto e = apply_hunk(content, op.hunks[i], i, dest, notes)) return *e;
            pl.staged[dest] = content;
            pl.order.push_back({dest, "Update", content.size()});
            return std::nullopt;
        }
        case OperationType::DELETE: {
            const auto st = fs::status(op.file_path, ec);
            if (ec || !fs::exists(st)) return "Delete File: " + op.file_path + " does not exist";
            if (fs::is_directory(st)) return "Delete File: " + op.file_path + " is a directory, not a file";
            if (pl.staged.count(op.file_path)) return "the patch deletes and writes " + op.file_path + "; split them";
            pl.removes.push_back(op.file_path);
            pl.order.push_back({op.file_path, "Delete", 0});
            return std::nullopt;
        }
        case OperationType::MOVE: {
            if (!fs::exists(op.file_path, ec)) return "Move File: " + op.file_path + " does not exist";
            if (fs::exists(dest, ec)) return "Move File: destination " + dest + " already exists";
            std::string content = fs_read_raw(op.file_path, opt.max_bytes, err);
            if (!err.empty()) return "Move File: " + op.file_path + ": " + err;
            for (size_t i = 0; i < op.hunks.size(); ++i)
                if (const auto e = apply_hunk(content, op.hunks[i], i, dest, notes)) return *e;
            pl.staged[dest] = content;
            pl.removes.push_back(op.file_path);
            pl.order.push_back({dest, "Move", content.size()});
            return std::nullopt;
        }
    }
    return op_label(op) + ": unsupported operation";
}

}  // namespace

ApplyReport apply_v4a_patch(const std::string& patch_text, const ApplyOptions& opt) {
    ApplyReport rep;
    const auto parsed = parse_v4a_patch(patch_text);
    if (parsed.second) { rep.error = "patch does not parse: " + *parsed.second; return rep; }
    if (parsed.first.empty()) { rep.error = "patch is empty"; return rep; }
    if (const auto e = dropped_hunks(patch_text)) { rep.error = *e; return rep; }

    Planned pl;
    for (const auto& op : parsed.first) {
        if (const auto e = plan_one(op, pl, opt, rep.notes)) {
            rep.error = *e;
            rep.notes.clear();   // nothing was committed; notes about hunks that matched are noise
            return rep;
        }
    }

    // Commit. Writes first (atomic rename per file), removals last, so a Move never loses the source
    // before the destination exists.
    for (auto& f : pl.order) {
        if (f.op == "Delete") continue;
        auto it = pl.staged.find(f.path);
        if (it == pl.staged.end()) continue;
        if (is_learned_skill(f.path, opt.skills_root)) it->second = annotate_edited(it->second, opt.origin, opt.now);
        const Json args = json_obj({{"path", jsp(json_str(f.path))}, {"content", jsp(json_str(it->second))}});
        Json res = json_parse(fs_write(args));
        if (const Json* e = json_get(res, "error")) {
            rep.error = std::string(op_name_for(f.op)) + " " + f.path + ": " + e->str +
                        (rep.files.empty() ? "" : "; " + std::to_string(rep.files.size()) + " file(s) already written");
            return rep;
        }
        f.bytes = it->second.size();
        rep.files.push_back(f);
    }
    for (const auto& p : pl.removes) {
        std::error_code ec;
        fs::remove(p, ec);
        if (ec) {
            rep.error = "removal failed: " + p + ": " + ec.message() +
                        (rep.files.empty() ? "" : "; " + std::to_string(rep.files.size()) + " file(s) already written");
            return rep;
        }
        rep.files.push_back({p, "Delete", 0});
    }
    return rep;
}

std::string fs_edit(const core::Json& args, ApplyOptions opt) {
    const Json* p = json_get(args, "patch");
    if (!p || p->type != Json::Str || p->str.empty()) return jerr("patch required");

    std::error_code ec;
    if (opt.root.empty()) {
        const fs::path cwd = fs::current_path(ec);
        if (!ec) opt.root = cwd.string();
    }
    if (opt.origin.empty()) opt.origin = get_current_write_origin();
    if (opt.now == 0) opt.now = static_cast<long long>(std::time(nullptr));
    if (const Json* d = json_get(args, "allow_delete"))
        opt.allow_delete = (d->type == Json::Bool && d->b) || (d->type == Json::Str && d->str == "true");

    const ApplyReport rep = apply_v4a_patch(p->str, opt);
    Json res = json_obj();
    obj_set(res, "applied", json_num(static_cast<double>(rep.files.size())));
    Json files = json_arr();
    for (const auto& f : rep.files) {
        Json one = json_obj();
        obj_set(one, "path", json_str(f.path));
        obj_set(one, "op", json_str(f.op));
        obj_set(one, "bytes", json_num(static_cast<double>(f.bytes)));
        arr_push(files, one);
    }
    obj_set(res, "files", files);
    if (!rep.notes.empty()) {
        Json notes = json_arr();
        for (const auto& n : rep.notes) arr_push(notes, json_str(n));
        obj_set(res, "notes", notes);
    }
    if (rep.error) obj_set(res, "error", json_str(*rep.error));
    return json_dump(res);
}

core::Json edit_tool_schema() {
    const std::string desc =
        "Edit files with a V4A patch. One call can add, update, move or delete several files.\n"
        "*** Begin Patch\n*** Add File: path\n+added line\n"
        "*** Update File: path\n@@ hint @@\n- removed line\n+ added line\n"
        "*** Delete File: path\n*** Move File: old -> new\n*** End Patch\n"
        "A @@ line names the function or anchor the hunk sits in; it is echoed back when a hunk fails."
        "A line starting with ' ' is context and must match the file. Every hunk is checked against the "
        "current file before anything is written, so a patch with one bad hunk changes nothing; the "
        "error names the file and hunk. Moves and deletes need allow_delete=\"true\".";
    Json props = json_obj();
    obj_set(props, "patch", json_obj({{"type", jsp(json_str("string"))},
                                      {"description", jsp(json_str("the whole V4A patch document"))}}));
    obj_set(props, "allow_delete", json_obj({{"type", jsp(json_str("string"))},
                                             {"description", jsp(json_str("\"true\" to let this patch move or delete a file"))}}));
    obj_set(props, "intent", json_obj({{"type", jsp(json_str("string"))},
                                       {"description", jsp(json_str("what you are doing, in a few words (optional)"))}}));
    Json req = json_arr();
    arr_push(req, json_str("patch"));
    Json parameters = json_obj({{"type", jsp(json_str("object"))},
                                {"properties", jsp(std::move(props))},
                                {"required", jsp(std::move(req))}});
    return json_obj({{"description", jsp(json_str(desc))}, {"parameters", jsp(std::move(parameters))}});
}

}  // namespace tools
}  // namespace dd
