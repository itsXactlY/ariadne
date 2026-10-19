// Differential test for dd::gateway::coerce_allow_set (authz_mixin.py pure subset).
#include "dd/gateway/authz_mixin.hpp"
#include "dd/core/json.hpp"
#include <iostream>
#include <set>
#include <string>
#include <vector>

using namespace dd;
using namespace dd::core;

int fails = 0;

static Json arr(std::initializer_list<Json> es) {
    std::vector<JsonSP> v;
    for (auto& e : es) v.push_back(std::make_shared<Json>(e));
    return json_arr(v);
}

static void check(const Json& raw, const std::vector<std::string>& want) {
    std::set<std::string> got = gateway::coerce_allow_set(raw);
    std::set<std::string> exp(want.begin(), want.end());
    if (got != exp) {
        ++fails;
        std::cout << "coerce: json_dump=" << json_dump(raw) << " got=[";
        bool f = true;
        for (auto& s : got) { if (!f) std::cout << ", "; f = false; std::cout << s; }
        std::cout << "] want=[";
        f = true;
        for (auto& s : exp) { if (!f) std::cout << ", "; f = false; std::cout << s; }
        std::cout << "]\n";
    }
}

int main() {
    check(json_null(), {});
    check(json_str("123,456"), {"123", "456"});
    check(json_str(" 123 , 456 ,"), {"123", "456"});
    check(json_str(""), {});
    check(json_str("a"), {"a"});
    check(json_str("1,2,,3"), {"1", "2", "3"});
    check(json_str("*,*"), {"*"});
    check(json_num(123.0), {"123"});
    check(json_bool(true), {"True"});
    check(json_str("x:8080/y"), {"x:8080/y"});
    check(json_arr(), {});
    check(arr({json_str("1"), json_str("2")}), {"1", "2"});
    check(arr({json_num(5), json_num(6)}), {"5", "6"});
    check(arr({json_bool(true), json_bool(false)}), {"False", "True"});
    check(arr({json_str(""), json_str("  "), json_str("x")}), {"x"});
    check(arr({json_num(1.5), json_num(2.5)}), {"1.5", "2.5"});
    check(arr({json_null()}), {"None"});
    // nested list element: str([1,2]) -> "[1, 2]"
    check(arr({arr({json_num(1), json_num(2)}), json_str("z")}), {"[1, 2]", "z"});
    // dict element: str({'k':1}) -> "{'k': 1}"
    {
        Json d = json_obj({{"k", std::make_shared<Json>(json_num(1))}});
        check(arr({json_str("a"), d}), {"a", "{'k': 1}"});
    }
    std::cout << "authz_mixin tests: " << (fails ? "FAIL" : "PASS")
              << " (" << fails << " failed)\n";
    return fails ? 1 : 0;
}
