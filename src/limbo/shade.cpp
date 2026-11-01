// src/limbo/shade.cpp — the contract's own names. They live here, not in a dweller's
// implementation: shade.hpp declares them, the judge and the front both read them, and a name
// that lives in Charon would make the pipe the owner of a word the whole level uses.
#include "dd/limbo/shade.hpp"

namespace dd {
namespace limbo {

const char* shade_kind_name(ShadeKind k) {
    switch (k) {
        case ShadeKind::Strata: return "strata";
        case ShadeKind::Llama: return "llama";
        case ShadeKind::None: return "none";
    }
    return "unknown";
}

}  // namespace limbo
}  // namespace dd
