// Pure subset of gateway/authz_mixin.py (see header for the porting boundary).
#include "dd/gateway/authz_mixin.hpp"
#include "dd/core/json.hpp"
#include <string>
#include <set>
#include <sstream>

namespace dd {
namespace gateway {

using core::Json;

static std::string strip_ws(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r' ||
                     s[a] == '\f' || s[a] == '\v'))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' ||
                     s[b - 1] == '\r' || s[b - 1] == '\f' || s[b - 1] == '\v'))
        --b;
    return s.substr(a, b - a);
}

// Python str() of a Json scalar/container. Containers render like repr()
// (Python str(list)/str(dict) delegate to repr of their elements).
static std::string py_repr_of(const Json& j);

static std::string py_str_of(const Json& j) {
    if (j.type == Json::Str) return j.str;
    return py_repr_of(j);
}

// Python repr() of a Json value: str -> 'quoted', others like str().
static std::string py_repr_of(const Json& j) {
    switch (j.type) {
        case Json::Null: return "None";
        case Json::Bool: return j.b ? "True" : "False";
        case Json::Num: {
            double v = j.num;
            if (v == (double)(long long)v && v >= -9.2e18 && v <= 9.2e18)
                return std::to_string((long long)v);
            std::ostringstream o;
            o << v;
            return o.str();
        }
        case Json::Str: {
            std::string out = "'";
            for (char c : j.str) {
                if (c == '\\' || c == '\'') { out.push_back('\\'); out.push_back(c); }
                else if (c == '\n') { out += "\\n"; }
                else out.push_back(c);
            }
            out.push_back('\'');
            return out;
        }
        case Json::Arr: {
            std::string out = "[";
            for (size_t i = 0; i < j.arr.size(); ++i) {
                if (i) out += ", ";
                out += py_repr_of(*j.arr[i]);
            }
            out.push_back(']');
            return out;
        }
        case Json::Obj: {
            std::string out = "{";
            for (size_t i = 0; i < j.obj.size(); ++i) {
                if (i) out += ", ";
                Json k;
                k.type = Json::Str;
                k.str = j.obj[i].first;
                out += py_repr_of(k);
                out += ": ";
                out += py_repr_of(*j.obj[i].second);
            }
            out.push_back('}');
            return out;
        }
    }
    return "";
}

std::set<std::string> coerce_allow_set(const Json& raw) {
    std::set<std::string> out;
    if (raw.type == Json::Null) return out;
    if (raw.type == Json::Arr) {
        for (const auto& e : raw.arr) {
            std::string s = strip_ws(py_str_of(*e));
            if (!s.empty()) out.insert(s);
        }
        return out;
    }
    std::string whole = py_str_of(raw);
    size_t pos = 0;
    while (true) {
        size_t c = whole.find(',', pos);
        std::string part = (c == std::string::npos)
                               ? whole.substr(pos)
                               : whole.substr(pos, c - pos);
        std::string sp = strip_ws(part);
        if (!sp.empty()) out.insert(sp);
        if (c == std::string::npos) break;
        pos = c + 1;
    }
    return out;
}

}  // namespace gateway
}  // namespace dd
