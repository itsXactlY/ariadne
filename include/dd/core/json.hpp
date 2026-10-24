#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dd {
namespace core {

// Minimal JSON value model: null, bool, number, string, array, object.
// Objects preserve insertion order (schemas need stable key order).
struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<std::shared_ptr<Json>> arr;
    std::vector<std::pair<std::string, std::shared_ptr<Json>>> obj;

    static Json make_null() { return Json{}; }
};

using JsonSP = std::shared_ptr<Json>;

// Containers nest at most this deep: the parser refuses more, dump and deep copy cut a deeper tree to null.
constexpr int kJsonMaxDepth = 256;

// Parse a JSON document (RFC 8259 numbers and strings); throws std::runtime_error on malformed input, on nesting
// past kJsonMaxDepth and on a number that overflows a double. Whatever follows the first value is ignored (SSE
// lines and tool-call arguments arrive with trailing text); inside a container the grammar is strict.
Json json_parse(const std::string& text);
// Serialize to compact JSON (UTF-8 passes through, ensure_ascii=false; an
// ill-formed byte goes out as U+FFFD, so a strict server always parses it).
std::string json_dump(const Json& v);
// Tolerant parse for tool-call `arguments`: a model that hits its output
// budget stops mid-value, leaving a string unterminated and containers open.
// Salvage the members that finished (close the containers they sat in) and set
// `repaired`; a document that parses as-is leaves it false. Never throws:
// Null + repaired=true means nothing was salvageable (nesting past kJsonMaxDepth included).
Json json_parse_args(const std::string& text, bool& repaired);

// Builders.
Json json_null();
Json json_bool(bool);
Json json_num(double);
Json json_str(std::string);
Json json_arr(std::vector<JsonSP> elems);
Json json_obj(std::vector<std::pair<std::string, JsonSP>> members);


// Empty-container builders.
Json json_arr();
Json json_obj();

// Mutating builders: append/replace, preserving insertion order.
void arr_push(Json& a, const Json& v);
void obj_set(Json& o, const std::string& key, const Json& v);
// Object accessors (return nullptr if key absent).
const Json* json_get(const Json& o, const std::string& key);

// Deep copy: fresh subtree (the Json copy ctor shares child pointers).
Json json_deep_copy(const Json& v);

}  // namespace core
}  // namespace dd
