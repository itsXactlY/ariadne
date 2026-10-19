// Differential test: agent/turn_context.py pure decision helpers vs Python truth.
#include "dd/agent/turn_context.hpp"
#include <iostream>

using namespace dd;
using namespace dd::agent;

static int g_fail = 0;
static void chk(const std::string& what, bool ok) {
    if (!ok) { std::cout << "FAIL " << what << "\n"; ++g_fail; }
}

int main() {
    // compression_made_progress(orig_len,new_len,orig_tokens,new_tokens)
    chk("cmp:sum", compression_made_progress(220, 220, 288000, 183000) == true);
    chk("cmp:wobble", compression_made_progress(220, 220, 288000, 280000) == false);
    chk("cmp:rows", compression_made_progress(10, 5, 1, 1) == true);
    chk("cmp:zero", compression_made_progress(10, 10, 0, 0) == false);
    chk("cmp:90", compression_made_progress(10, 10, 100, 90) == true);
    chk("cmp:95", compression_made_progress(10, 10, 100, 95) == false);
    chk("cmp:94", compression_made_progress(10, 10, 100, 94) == true);
    chk("cmp:96", compression_made_progress(10, 10, 100, 96) == false);
    chk("cmp:all0", compression_made_progress(0, 0, 0, 0) == false);
    chk("cmp:1to0", compression_made_progress(5, 5, 1, 0) == true);

    // compression_warrants_another_preflight_pass(orig,new,threshold)
    chk("war:over", compression_warrants_another_preflight_pass(288000, 183000, 100000) == true);
    chk("war:under", compression_warrants_another_preflight_pass(288000, 90000, 100000) == false);
    chk("war:94", compression_warrants_another_preflight_pass(100, 94, 50) == true);
    chk("war:95", compression_warrants_another_preflight_pass(100, 95, 50) == false);
    chk("war:zero", compression_warrants_another_preflight_pass(0, 0, 0) == false);
    chk("war:eq", compression_warrants_another_preflight_pass(5, 5, 5) == false);
    chk("war:notmat", compression_warrants_another_preflight_pass(100, 100, 50) == false);
    chk("war:thr95", compression_warrants_another_preflight_pass(100, 94, 95) == false);

    if (g_fail) { std::cout << "turn_context: FAIL " << g_fail << "\n"; return 1; }
    std::cout << "all turn_context tests passed\n";
    return 0;
}
