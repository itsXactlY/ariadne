// Faithful port of tools/todo_tool.py (TodoStore + todo_tool).
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "dd/core/json.hpp"
#include "dd/tools/todo_tool.hpp"

using namespace dd;
using namespace dd::core;

namespace dd {
namespace tools {

static const std::set<std::string> VALID_STATUSES = {
    "pending", "in_progress", "completed", "cancelled"};

// Python str.strip() for ASCII whitespace.
static std::string py_strip(const std::string& s) {
    size_t a = 0, b = s.size();
    auto ws = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    while (a < b && ws(s[a])) ++a;
    while (b > a && ws(s[b - 1])) --b;
    return s.substr(a, b - a);
}

// ASCII lower (statuses are ASCII).
static std::string py_lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Python truthiness of a JSON value.
static bool json_truthy(const Json* v) {
    if (!v) return false;
    switch (v->type) {
        case Json::Null: return false;
        case Json::Bool: return v->b;
        case Json::Num: return v->num != 0.0;
        case Json::Str: return !v->str.empty();
        case Json::Arr: return !v->arr.empty();
        case Json::Obj: return !v->obj.empty();
    }
    return false;
}

// Python str() over a JSON value. Faithful for Str/Bool/Null/integral Num;
// containers fall back to their serialized form (never used for real
// todo payloads, which carry string id/content/status).
static std::string py_str(const Json* v) {
    if (!v) return "";
    switch (v->type) {
        case Json::Null: return "None";
        case Json::Bool: return v->b ? "True" : "False";
        case Json::Str: return v->str;
        case Json::Num: {
            if (v->num == (double)(long long)v->num) {
                std::ostringstream os;
                os << (long long)v->num;
                return os.str();
            }
            return json_dump(*v);
        }
        case Json::Arr:
        case Json::Obj:
            return json_dump(*v);
    }
    return "";
}

// item.get(key, default) as a str: default applies when the key is absent.
static std::string get_str(const Json& o, const std::string& key,
                           const std::string& def) {
    const Json* v = json_get(o, key);
    if (!v) return def;
    return py_str(v);
}

TodoItem TodoStore::validate(const core::Json& item) {
    std::string item_id = py_strip(get_str(item, "id", ""));
    if (item_id.empty()) item_id = "?";

    std::string content = py_strip(get_str(item, "content", ""));
    if (content.empty()) content = "(no description)";

    std::string status = py_lower(py_strip(get_str(item, "status", "pending")));
    if (!VALID_STATUSES.count(status)) status = "pending";

    return TodoItem{item_id, content, status};
}

std::vector<TodoItem> TodoStore::read() const { return items; }

bool TodoStore::has_items() const { return !items.empty(); }

std::vector<TodoItem> TodoStore::write(const core::Json& todos_arr, bool merge) {
    if (todos_arr.type != Json::Arr) {
        items.clear();
        return read();
    }

    if (!merge) {
        std::vector<TodoItem> next;
        for (const auto& sp : todos_arr.arr) next.push_back(validate(*sp));
        items = std::move(next);
        return read();
    }

    // merge=True: index current items by id (last wins on duplicates).
    std::map<std::string, TodoItem> existing;
    for (const auto& it : items) existing[it.id] = it;

    for (const auto& sp : todos_arr.arr) {
        const core::Json& t = *sp;
        std::string item_id = py_strip(get_str(t, "id", ""));
        if (item_id.empty()) continue;

        auto it = existing.find(item_id);
        if (it != existing.end()) {
            const Json* c = json_get(t, "content");
            if (c && json_truthy(c)) {
                it->second.content = py_strip(py_str(c));
            }
            const Json* s = json_get(t, "status");
            if (s && json_truthy(s)) {
                std::string st = py_lower(py_strip(py_str(s)));
                if (VALID_STATUSES.count(st)) it->second.status = st;
            }
        } else {
            TodoItem validated = validate(t);
            existing[validated.id] = validated;
            items.push_back(validated);
        }
    }

    std::set<std::string> seen;
    std::vector<TodoItem> rebuilt;
    for (const auto& it : items) {
        auto e = existing.find(it.id);
        const TodoItem& current = (e != existing.end()) ? e->second : it;
        if (!seen.count(current.id)) {
            rebuilt.push_back(current);
            seen.insert(current.id);
        }
    }
    items = std::move(rebuilt);
    return read();
}

std::optional<std::string> TodoStore::format_for_injection() const {
    if (items.empty()) return std::nullopt;

    std::vector<TodoItem> active;
    for (const auto& it : items) {
        if (it.status == "pending" || it.status == "in_progress") active.push_back(it);
    }
    if (active.empty()) return std::nullopt;

    auto marker = [](const std::string& status) -> std::string {
        if (status == "completed") return "[x]";
        if (status == "in_progress") return "[>]";
        if (status == "pending") return "[ ]";
        if (status == "cancelled") return "[~]";
        return "[?]";
    };

    std::vector<std::string> lines;
    lines.push_back("[Your active task list was preserved across context compression]");
    for (const auto& it : active) {
        lines.push_back("- " + marker(it.status) + " " + it.id + ". " + it.content +
                        " (" + it.status + ")");
    }

    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) out += "\n";
        out += lines[i];
    }
    return out;
}

// Serialize a string exactly as json.dumps(..., ensure_ascii=False) would.
static std::string esc_str(const std::string& s) {
    Json j;
    j.type = Json::Str;
    j.str = s;
    return json_dump(j);
}

std::string todo_tool(const core::Json& todos, bool merge, TodoStore& store) {
    std::vector<TodoItem> items;
    if (todos.type != Json::Null) {
        items = store.write(todos, merge);
    } else {
        items = store.read();
    }

    int pending = 0, in_progress = 0, completed = 0, cancelled = 0;
    for (const auto& it : items) {
        if (it.status == "pending") ++pending;
        else if (it.status == "in_progress") ++in_progress;
        else if (it.status == "completed") ++completed;
        else if (it.status == "cancelled") ++cancelled;
    }

    // json.dumps default separators: ", " between members, ": " after keys.
    std::string todo_json_array = "[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) todo_json_array += ", ";
        todo_json_array += "{\"id\": " + esc_str(items[i].id) +
                           ", \"content\": " + esc_str(items[i].content) +
                           ", \"status\": " + esc_str(items[i].status) + "}";
    }
    todo_json_array += "]";

    std::string summary = "{\"total\": " + std::to_string((int)items.size()) +
                          ", \"pending\": " + std::to_string(pending) +
                          ", \"in_progress\": " + std::to_string(in_progress) +
                          ", \"completed\": " + std::to_string(completed) +
                          ", \"cancelled\": " + std::to_string(cancelled) + "}";

    return "{\"todos\": " + todo_json_array + ", \"summary\": " + summary + "}";
}

}  // namespace tools
}  // namespace dd
