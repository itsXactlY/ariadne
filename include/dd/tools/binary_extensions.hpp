// tools/binary_extensions.py -> dd::tools
#ifndef DD_TOOLS_BINARY_EXTENSIONS_HPP
#define DD_TOOLS_BINARY_EXTENSIONS_HPP
#include <string>

namespace dd {
namespace tools {

// has_binary_extension: pure string check, no I/O.
bool has_binary_extension(const std::string& path);

}  // tools
}  // dd
#endif
