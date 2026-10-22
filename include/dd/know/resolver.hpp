#pragma once
// dd/know/resolver.hpp — what the model can do with a pointer: one verb for every kind.
//
//   read_file("skill://name")   a skill, a page at a time (#2, #all), or its own files (skill://name/references/x.md)
//   read_file("mem://1290233")  a memory row, in full
//   find(query, kinds?)         what the catalogues hold for these words, strongest first, with how sure it is
//   skill_load(name)            the old way in: the same as read_file("skill://name")
//
// Every open is recorded: it is how the router knows a pointer was followed, and which skill the
// model is working from when the window is rebuilt.

#include <optional>
#include <string>

#include "dd/core/json.hpp"
#include "dd/know/router.hpp"

namespace dd {
namespace know {

class Resolver {
public:
    explicit Resolver(Router& r) : router_(r) {}

    // read_file's path when it is an internal uri: the JSON result; nullopt when it is a file path.
    std::optional<std::string> read(const std::string& path);
    std::string find(const core::Json& args) const;
    std::string skill_load(const core::Json& args);

    static core::Json find_schema();         // {description, parameters} for the registry
    static core::Json skill_load_schema();

private:
    Router& router_;
    std::string opened_json(const Uri& u, const Body& b) const;
    std::string missing_json(const Uri& u) const;
};

}  // namespace know
}  // namespace dd
