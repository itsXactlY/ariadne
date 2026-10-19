#pragma once
// Pure parsing subsystem of tools/osv_check.py: command/args -> (ecosystem, package, version).
// The HTTP OSV query (_query_osv) and malware-message formatting are NOT ported here.
#include <string>
#include <optional>
#include <vector>
#include <utility>
#include <tuple>

namespace dd {
namespace tools {

std::optional<std::string> infer_ecosystem(const std::string& command);

std::pair<std::optional<std::string>, std::optional<std::string>>
parse_package_from_args(const std::vector<std::string>& args, const std::string& ecosystem);

std::pair<std::optional<std::string>, std::optional<std::string>>
parse_npm_package(const std::string& token);

std::pair<std::optional<std::string>, std::optional<std::string>>
parse_pypi_package(const std::string& token);

// Pure prefix of check_package_for_malware: returns (ecosystem, package, version)
// when the command/args parse, else std::nullopt. No network.
std::optional<std::tuple<std::string, std::optional<std::string>, std::optional<std::string>>>
parse_package(const std::string& command, const std::vector<std::string>& args);

}  // namespace tools
}  // namespace dd
