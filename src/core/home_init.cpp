// dd::core::init_config — a fresh home gets a config.yaml (the embedded reference, or a copy of one).
#include "dd/core/home.hpp"

#include <fstream>
#include <iterator>
#include <system_error>

namespace dd {
namespace core {

bool init_config(const Home& h, const std::string& from, std::string* why) {
    std::error_code ec;
    if (std::filesystem::exists(h.config(), ec)) {
        if (why) *why = h.config().string() + " exists";
        return false;
    }
    std::string text;
    if (!from.empty()) {
        std::ifstream in(from, std::ios::binary);
        if (!in) {
            if (why) *why = "cannot read " + from;
            return false;
        }
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    } else {
        text = default_config();
    }
    std::filesystem::create_directories(h.root, ec);
    std::ofstream out(h.config(), std::ios::binary);
    if (!out) {
        if (why) *why = "cannot write " + h.config().string();
        return false;
    }
    out << text;
    return static_cast<bool>(out);
}

}  // namespace core
}  // namespace dd
