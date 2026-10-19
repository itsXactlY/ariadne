// Pure parsing subsystem of tools/osv_check.py (no HTTP).
#include "dd/tools/osv_package_parse.hpp"

#include <string>
#include <optional>
#include <vector>
#include <utility>
#include <tuple>
#include <regex>
#include <algorithm>
#include <cctype>

namespace dd {
namespace tools {

static std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::optional<std::string> infer_ecosystem(const std::string& command) {
    // os.path.basename(command).lower()
    auto pos = command.rfind('/');
    std::string base = (pos == std::string::npos) ? command : command.substr(pos + 1);
    base = to_lower(base);
    if (base == "npx" || base == "npx.cmd") return std::string("npm");
    if (base == "uvx" || base == "uvx.cmd" || base == "pipx") return std::string("PyPI");
    return std::nullopt;
}

std::pair<std::optional<std::string>, std::optional<std::string>>
parse_npm_package(const std::string& token) {
    if (!token.empty() && token[0] == '@') {
        static const std::regex re(R"(^(@[^/]+/[^@]+)(?:@(.+))?$)");
        std::smatch m;
        if (std::regex_match(token, m, re)) {
            std::optional<std::string> g1 = m[1].matched
                ? std::optional<std::string>(m[1].str()) : std::nullopt;
            std::optional<std::string> g2 = m[2].matched
                ? std::optional<std::string>(m[2].str()) : std::nullopt;
            return { g1, g2 };
        }
        return { token, std::nullopt };
    }
    if (token.find('@') != std::string::npos) {
        auto pos = token.rfind('@');
        std::string name = token.substr(0, pos);
        std::string v = token.substr(pos + 1);
        std::optional<std::string> version = (v != "latest")
            ? std::optional<std::string>(v) : std::nullopt;
        return { name, version };
    }
    return { token, std::nullopt };
}

std::pair<std::optional<std::string>, std::optional<std::string>>
parse_pypi_package(const std::string& token) {
    static const std::regex re(R"(^([a-zA-Z0-9._-]+)(?:\[[^\]]*\])?(?:==(.+))?$)");
    std::smatch m;
    if (std::regex_match(token, m, re)) {
        std::optional<std::string> g1 = m[1].matched
            ? std::optional<std::string>(m[1].str()) : std::nullopt;
        std::optional<std::string> g2 = m[2].matched
            ? std::optional<std::string>(m[2].str()) : std::nullopt;
        return { g1, g2 };
    }
    return { token, std::nullopt };
}

std::pair<std::optional<std::string>, std::optional<std::string>>
parse_package_from_args(const std::vector<std::string>& args, const std::string& ecosystem) {
    if (args.empty()) return { std::nullopt, std::nullopt };
    std::string package_token;
    bool found = false;
    for (const std::string& arg : args) {
        if (!arg.empty() && arg[0] == '-') continue;  // arg.startswith("-")
        package_token = arg;
        found = true;
        break;
    }
    if (!found || package_token.empty()) return { std::nullopt, std::nullopt };
    if (ecosystem == "npm") return parse_npm_package(package_token);
    if (ecosystem == "PyPI") return parse_pypi_package(package_token);
    return { package_token, std::nullopt };
}

std::optional<std::tuple<std::string, std::optional<std::string>, std::optional<std::string>>>
parse_package(const std::string& command, const std::vector<std::string>& args) {
    auto eco = infer_ecosystem(command);
    if (!eco) return std::nullopt;
    auto [pkg, ver] = parse_package_from_args(args, *eco);
    if (!pkg || pkg->empty()) return std::nullopt;  // if not package: return None
    return std::make_tuple(*eco, pkg, ver);
}

}  // namespace tools
}  // namespace dd
