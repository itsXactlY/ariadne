// src/context/render.cpp — impl of dd/context/render.hpp.
#include "dd/context/render.hpp"

#include "dd/core/json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>

namespace dd {
namespace context {

using core::Json;
using core::Message;

namespace {

const char* const kIdArgKeys[] = {"file_path", "path", "command", "cmd", "pattern",
                                  "query", "url", "name", "target", "expression"};

std::string squash(const std::string& s) {   // runs of whitespace -> one space
    std::string o;
    bool sp = false;
    for (char c : s) {
        const bool ws = c == ' ' || c == '\n' || c == '\t' || c == '\r';
        if (ws) {
            sp = !o.empty();
            continue;
        }
        if (sp) o.push_back(' ');
        sp = false;
        o.push_back(c);
    }
    return o;
}

size_t char_floor(const std::string& s, size_t n) {   // never split a UTF-8 sequence
    n = std::min(n, s.size());
    while (n > 0 && n < s.size() && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return n;
}

const core::ToolCall* call_for(const std::vector<ItemPtr>& view, const std::string& id, int& siblings) {
    siblings = 1;
    if (id.empty()) return nullptr;
    for (size_t i = view.size(); i-- > 0;) {
        const Message& m = view[i]->msg;
        for (const auto& c : m.tool_calls)
            if (c.id == id) {
                siblings = static_cast<int>(m.tool_calls.size());
                return &c;
            }
    }
    return nullptr;
}

// The text a result carries, without its JSON envelope.
std::string readable(const std::string& content) {
    try {
        const Json j = core::json_parse(content);
        if (j.type == Json::Obj)
            for (const char* key : {"output", "content", "result", "text", "stdout"})
                if (const Json* v = core::json_get(j, key);
                    v && v->type == Json::Str && static_cast<double>(v->str.size()) >= 0.8 * static_cast<double>(content.size()))
                    return v->str;
    } catch (...) {
    }
    return content;
}

std::vector<ItemPtr> window(const std::vector<ItemPtr>& items, long current, int turns) {
    if (turns < 0 || items.size() <= 2) return items;
    const long cur = current >= 0 ? current : static_cast<long>(items.size()) - 1;
    const long head = std::max(0L, cur - static_cast<long>(turns) * 2);
    std::vector<ItemPtr> out(items.begin() + head, items.end());
    if (out.size() > 1 && out.front()->msg.role == "system") out.erase(out.begin());
    return out;
}

}  // namespace

std::string Spill::write(const std::string& kind, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    if (ec) return "";
    std::string safe;
    for (char c : kind) safe.push_back(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ? c : '_');
    char name[64];
    std::snprintf(name, sizeof name, "%04u-%s.txt", ++seq_, safe.empty() ? "spill" : safe.substr(0, 40).c_str());
    const std::string path = dir_ + "/" + name;
    std::ofstream f(path, std::ios::binary);
    if (!f) return "";
    f << text;
    return f ? path : "";
}

std::string Renderer::describe_call(const core::ToolCall& c) {
    const std::string name = c.name.empty() ? "tool" : c.name;
    std::string arg;
    const std::string& raw = c.arguments;
    if (!raw.empty()) {
        try {
            const Json j = core::json_parse(raw);
            if (j.type == Json::Obj) {
                for (const char* key : kIdArgKeys)
                    if (const Json* v = core::json_get(j, key); v && v->type == Json::Str && !squash(v->str).empty()) {
                        arg = v->str;
                        break;
                    }
                if (arg.empty())
                    for (const auto& [k, v] : j.obj)
                        if (v && v->type == Json::Str && !squash(v->str).empty()) {
                            arg = v->str;
                            break;
                        }
            } else if (squash(raw) != "{}") {
                arg = raw;
            }
        } catch (...) {
            if (squash(raw) != "{}") arg = raw;
        }
    }
    arg = squash(arg);
    if (arg.size() > 60) arg = arg.substr(0, char_floor(arg, 57)) + "...";
    return name + "(" + arg + ")";
}

std::vector<Message> Renderer::sanitize(std::vector<Message> msgs) {
    msgs.erase(std::remove_if(msgs.begin(), msgs.end(), [](const Message& m) {
                   return m.role != "system" && m.role != "user" && m.role != "assistant" && m.role != "tool";
               }),
               msgs.end());
    std::set<std::string> calls, results;
    for (const auto& m : msgs) {
        if (m.role == "assistant")
            for (const auto& c : m.tool_calls)
                if (!c.id.empty()) calls.insert(c.id);
        if (m.role == "tool" && !m.tool_call_id.empty()) results.insert(m.tool_call_id);
    }
    msgs.erase(std::remove_if(msgs.begin(), msgs.end(), [&](const Message& m) {
                   return m.role == "tool" && !m.tool_call_id.empty() && !calls.count(m.tool_call_id);
               }),
               msgs.end());
    bool missing = false;
    for (const auto& id : calls) missing = missing || !results.count(id);
    if (!missing) return msgs;
    std::vector<Message> out;
    for (auto& m : msgs) {
        out.push_back(m);
        if (m.role != "assistant") continue;
        for (const auto& c : m.tool_calls)
            if (!c.id.empty() && !results.count(c.id)) {
                Message stub;
                stub.role = "tool";
                stub.tool_call_id = c.id;
                stub.content = "[Result unavailable — see context summary above]";
                out.push_back(stub);
            }
    }
    return out;
}

void Renderer::note_ratio(long real_tokens, long estimated_tokens) {
    if (real_tokens > 0 && estimated_tokens > 1000)
        ratio_ = std::min(2.5, std::max(0.8, static_cast<double>(real_tokens) / static_cast<double>(estimated_tokens)));
}

long Renderer::budget_chars(int siblings) const {
    if (p_.context_length <= 0) return 0;
    const double share = std::min(0.10, 0.40 / std::max(1, siblings));
    const double chars = static_cast<double>(p_.context_length) * share * 4.0 / ratio_;
    return std::max(static_cast<long>(kExcerptMin), static_cast<long>(chars));
}

std::string Renderer::excerpt(const std::string& text, const std::string& path, const std::string& tool,
                              long budget, const Json* read_args, long total_lines) const {
    std::string head = text.substr(0, char_floor(text, static_cast<size_t>(static_cast<double>(budget) * 0.85)));
    if (const size_t nl = head.rfind('\n'); nl != std::string::npos && nl > head.size() / 2) head.resize(nl);
    const long lines = std::max(1L, static_cast<long>(std::count(head.begin(), head.end(), '\n')) + 1);
    const long page = std::max(50L, static_cast<long>(static_cast<double>(lines) * 0.9));
    const std::string name = tool.empty() ? "tool" : tool;
    const std::string marker = std::string("\n\n") + kResultCut + " - NOT REAL CONTENT, do not copy this marker. ";
    if (read_args) {
        long start = 1, asked = 500;
        if (const Json* o = core::json_get(*read_args, "offset"); o && o->type == Json::Num) start = static_cast<long>(o->num);
        if (const Json* l = core::json_get(*read_args, "limit"); l && l->type == Json::Num) asked = static_cast<long>(l->num);
        long next = start + lines;
        const std::string last = head.substr(head.rfind('\n') == std::string::npos ? 0 : head.rfind('\n') + 1);
        size_t k = 0;
        while (k < last.size() && last[k] == ' ') ++k;
        size_t d = k;
        while (d < last.size() && std::isdigit(static_cast<unsigned char>(last[d]))) ++d;
        if (d > k && d < last.size() && last[d] == '|') next = std::stol(last.substr(k, d - k)) + 1;
        long stop = start + asked - 1;
        if (total_lines > 0) stop = std::min(stop, total_lines);
        const Json* p = core::json_get(*read_args, "path");
        return head + marker + "Lines " + std::to_string(next) + "-" + std::to_string(stop) +
               " of this read did not fit this window. Continue with read_file(path=\"" + (p ? p->str : "") +
               "\", offset=" + std::to_string(next) + ", limit=" + std::to_string(page) +
               "), part by part, and keep notes of what matters.]]";
    }
    if (path.empty())
        return head + marker + name + " returned " + std::to_string(text.size()) + " chars; this window holds the first " +
               std::to_string(head.size()) +
               ". For the rest run a narrower call (a slice, a grep, less printed) and keep notes of what matters.]]";
    return head + marker + name + " returned " + std::to_string(text.size()) + " chars; this window holds the first " +
           std::to_string(head.size()) + ". The whole output is read_file(\"" + path + "\"): continue at offset=" +
           std::to_string(lines + 1) + " with limit=" + std::to_string(page) +
           ", part by part, and keep notes of what matters. Do not run the call again.]]";
}

std::string Renderer::capped(const Message& tool, const std::vector<ItemPtr>& view) const {
    const std::string& content = tool.content;
    if (content.size() <= kExcerptMin) return "";
    if (content.find("[SPILLED RESULT") != std::string::npos || content.find(kResultCut) != std::string::npos) return "";
    int siblings = 1;
    const core::ToolCall* call = call_for(view, tool.tool_call_id, siblings);
    const long budget = budget_chars(siblings);
    if (!budget || static_cast<long>(content.size()) <= budget) return "";
    const std::string name = call ? call->name : "";
    Json args = core::json_obj();
    if (call) try { args = core::json_parse(call->arguments.empty() ? "{}" : call->arguments); } catch (...) {}
    const std::string text = readable(content);
    if (name == "read_file" && args.type == Json::Obj && core::json_get(args, "path")) {
        long total = 0;
        try {
            const Json j = core::json_parse(content);
            if (const Json* t = core::json_get(j, "total_lines"); t && t->type == Json::Num) total = static_cast<long>(t->num);
        } catch (...) {
        }
        return excerpt(text, "", name, budget, &args, total);
    }
    if (maze_up_ && maze_up_()) return excerpt(text, "", name, budget, nullptr, 0);   // retrieval is the memory
    if (!spill_) return "";
    const std::string path = spill_->write(name.empty() ? "tool-result" : name, text);
    return path.empty() ? "" : excerpt(text, path, name, budget, nullptr, 0);
}

std::vector<Message> Renderer::one(Item& item, bool is_current, const std::string& injection,
                                   const std::vector<ItemPtr>& view) {
    Message m = item.msg;
    if (m.role == "tool")
        if (std::string c = capped(m, view); !c.empty()) m.content = std::move(c);
    if (is_current && m.role == "user" && !injection.empty()) m.content += "\n\n" + injection;
    if (m.role == "assistant" && soak_) soak_(item);   // the maze gets the thinking the first time it is sent
    return {m};
}

Renderer::Epoch Renderer::epoch(const std::vector<ItemPtr>& items, long current, const std::string& injection) {
    const Item* cur_item = current >= 0 && current < static_cast<long>(items.size()) ? items[static_cast<size_t>(current)].get() : nullptr;
    Epoch ep;
    ep.sources = window(items, current, p_.window_turns);
    const bool maze = maze_up_ && maze_up_();

    struct W {
        Item* src;
        Message m;
    };
    std::vector<W> work;
    work.reserve(ep.sources.size());
    for (const auto& it : ep.sources) work.push_back(W{it.get(), it->msg});

    // Old results to disk, only with the maze dead (the newest two stay).
    if (!maze && spill_) {
        int seen = 0;
        for (size_t i = work.size(); i-- > 0;) {
            Message& m = work[i].m;
            if (m.role != "tool") continue;
            if (++seen <= 2 || m.content.size() < 800 || m.content.find("[SPILLED RESULT") != std::string::npos) continue;
            int sib = 1;
            const core::ToolCall* call = call_for(ep.sources, m.tool_call_id, sib);
            const std::string tool = call ? call->name : "tool";
            const std::string path = spill_->write(tool, readable(m.content));
            if (!path.empty())
                m.content = "[SPILLED RESULT — " + tool + " output (" + std::to_string(m.content.size()) +
                            " chars) moved to disk. Read it back with read_file(\"" + path + "\").]";
        }
    }

    // Finished tool exchanges past the TTL leave as one line per group.
    {
        std::vector<std::pair<size_t, size_t>> groups;
        for (size_t i = 0; i < work.size();) {
            const Message& m = work[i].m;
            if (m.role != "assistant" || m.tool_calls.empty()) { ++i; continue; }
            std::set<std::string> ids;
            for (const auto& c : m.tool_calls) ids.insert(c.id);
            size_t end = i + 1;
            while (end < work.size() && work[end].m.role == "tool" && ids.count(work[end].m.tool_call_id)) ++end;
            groups.emplace_back(i, end);
            i = end;
        }
        const size_t ttl = static_cast<size_t>(std::max(0, p_.tool_group_ttl));
        if (groups.size() > ttl) {
            std::vector<W> out;
            size_t g = 0;
            const size_t doomed = groups.size() - ttl;
            for (size_t i = 0; i < work.size();) {
                if (g < doomed && groups[g].first == i) {
                    const auto [start, end] = groups[g++];
                    std::string lines;
                    bool paths = false;
                    for (const auto& c : work[start].m.tool_calls) {
                        std::string line = describe_call(c);
                        for (size_t r = start + 1; r < end; ++r)
                            if (work[r].m.tool_call_id == c.id) {
                                const std::string& rc = work[r].m.content;
                                if (const size_t q = rc.find("read_file(\""); q != std::string::npos) {
                                    const size_t e = rc.find('"', q + 11);
                                    if (e != std::string::npos) {
                                        line += " -> " + rc.substr(q + 11, e - q - 11);
                                        paths = true;
                                    }
                                }
                                break;
                            }
                        lines += (lines.empty() ? "" : "; ") + line;
                    }
                    if (work[start].src && soak_) soak_(*work[start].src);   // its thinking leaves with it
                    if (!lines.empty()) {
                        Message line;
                        line.role = "assistant";
                        line.content = "[aged out of context] " + lines + "." +
                                       (paths ? " Paths are readable with read_file(path)." : "") +
                                       " Run a call again if its output is needed.";
                        out.push_back(W{nullptr, std::move(line)});
                    }
                    i = end;
                    continue;
                }
                out.push_back(std::move(work[i]));
                ++i;
            }
            work = std::move(out);
        }
    }

    // Reasoning past its window: dropped (the maze has it), on disk only with the maze dead.
    {
        int fresh = 0;
        for (size_t i = work.size(); i-- > 0;) {
            Message& m = work[i].m;
            if (m.role != "assistant" || m.reasoning.empty()) continue;
            if (fresh < p_.reasoning_window) { ++fresh; continue; }
            if (m.reasoning.size() < kReasoningSpillMin) continue;   // smaller than its handle
            if (maze) {
                if (work[i].src && soak_) soak_(*work[i].src);
                m.reasoning.clear();
                continue;
            }
            const std::string path = spill_ ? spill_->write("reasoning", m.reasoning) : "";
            if (!path.empty())
                m.reasoning = "[earlier reasoning (" + std::to_string(m.reasoning.size()) +
                              " chars) is on disk: read_file(\"" + path + "\")]";
        }
    }

    // this turn's thinking is new at an epoch start
    bool after = false;
    for (auto& w : work) {
        if (w.src == cur_item) after = true;
        else if (after && w.src && w.m.role == "assistant" && soak_) soak_(*w.src);
    }
    for (auto& w : work) {
        Message& m = w.m;
        if (m.role == "tool")
            if (std::string c = capped(m, ep.sources); !c.empty()) m.content = std::move(c);
        if (w.src && w.src == cur_item && m.role == "user" && !injection.empty()) m.content += "\n\n" + injection;
        ep.rendered.push_back(std::move(m));
    }
    ep.rendered = sanitize(std::move(ep.rendered));
    return ep;
}

}  // namespace context
}  // namespace dd
