#pragma once
// dd/core/par.hpp — how many OpenMP threads a parallel region may use.
//
// At most 8: past that a region is memory-bound, and the box's other cores belong to the
// model. Every region carries an if() with a size below which it does not open at all, so
// idle work never wakes threads (see ui.cpp, kParCells).

#include <algorithm>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace dd {
namespace core {

inline int par_threads() {
#ifdef _OPENMP
    return std::min(8, omp_get_max_threads());
#else
    return 1;
#endif
}

}  // namespace core
}  // namespace dd
