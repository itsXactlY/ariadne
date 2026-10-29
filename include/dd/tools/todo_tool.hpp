#pragma once
// Faithful port of tools/todo_tool.py — the pure in-memory TodoStore and
// todo_tool entry point. No fs/subprocess/http coupling; JSON output matches
// json.dumps(..., ensure_ascii=False) key order.
#include <optional>
#include <string>
#include <vector>

#include "dd/core/json.hpp"

namespace dd {
namespace tools {

struct TodoItem {
    std::string id;
    std::string content;
    std::string status;
};

class TodoStore {
public:
    std::vector<TodoItem> items;

    // Mirrors TodoStore.write(todos, merge); returns the post-write read().
    std::vector<TodoItem> write(const core::Json& todos_arr, bool merge);
    // Mirrors read(): a copy of the current list.
    std::vector<TodoItem> read() const;
    bool has_items() const;
    // Mirrors format_for_injection(); None when empty or no active items.
    std::optional<std::string> format_for_injection() const;

    // Mirrors TodoStore._validate (staticmethod).
    static TodoItem validate(const core::Json& item);
};

// Mirrors todo_tool(todos, merge, store). `todos` is a Json array to write;
// a Null Json means "not provided" (read). Returns the JSON result string.
std::string todo_tool(const core::Json& todos, bool merge, TodoStore& store);

// The `merge` argument as a model writes it. The schema says boolean, but a model that sends
// "true" / "True" as a string means it; reading that as false replaces the whole list with
// content-less items. Absent, null, or anything that does not say yes is false.
bool todo_merge_flag(const core::Json* v);

}  // namespace tools
}  // namespace dd
