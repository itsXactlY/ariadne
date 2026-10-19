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

}  // namespace tools
}  // namespace dd
