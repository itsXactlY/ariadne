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

// Parse a JSON document; throws std::runtime_error on malformed input.
Json json_parse(const std::string& text);
// Serialize to compact JSON (UTF-8 bytes pass through, ensure_ascii=false).
std::string json_dump(const Json& v);

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
