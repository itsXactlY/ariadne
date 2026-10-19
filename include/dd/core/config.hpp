#pragma once
// dd/core/config.hpp — general YAML loader for ~/.daedalus/config.yaml.
//
// Parses the block-style YAML subset the config uses (nested maps, block
// lists at the key's indent OR deeper, inline [] / {}, single/double-quoted
// scalars with \uXXXX + line-continuation, bare scalars, ints/floats/bools,
// # comments) into the shared core::Json value tree:
//   mapping  -> Json::Obj (insertion-ordered)
//   sequence -> Json::Arr
//   scalar   -> Json::Str / Num / Bool / Null
//
// Config then exposes typed getters addressed by a dotted path
// ("model.name", "agent.max_turns", "toolsets", "_config_version").

#include <string>
#include "dd/core/json.hpp"

namespace dd {
namespace core {

// Parse a YAML document into the Json tree. Returns Json::Null on empty input.
Json parse_yaml(const std::string& text);

class Config {
public:
    Json root;  // Json::Obj at the top level

    Config(const Json& r) : root(r) {}

    // Raw node at a dotted path (nullptr if absent).
    const Json* get(const std::string& path) const;

    bool has(const std::string& path) const;

    // Typed accessors. Missing / wrong-type -> default.
    std::string get_string(const std::string& path) const;
    long long get_int(const std::string& path) const;
    double get_double(const std::string& path) const;
    bool get_bool(const std::string& path) const;

    // Container accessors: pointer into the tree (nullptr if absent / wrong kind).
    const Json* get_map(const std::string& path) const;
    const Json* get_list(const std::string& path) const;
};

// Read + parse a YAML file. Throws std::runtime_error if the file can't be read.
Config load_config(const std::string& path);

}  // namespace core
}  // namespace dd
