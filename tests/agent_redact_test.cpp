// Differential test: agent/redact.py _mask_token pure subset vs Python truth.
#include "dd/agent/redact.hpp"
#include <string>
#include <iostream>

using namespace dd;
using namespace dd::agent;

static int g_fail = 0;
static void chk(const std::string& what, bool ok) {
    if (!ok) { std::cout << "FAIL " << what << "\n"; ++g_fail; }
}

int main() {
    chk("short", mask_token("") == "***");
    chk("short2", mask_token("short") == "***");
    chk("len17", mask_token("12345678901234567") == "***");
    chk("len18", mask_token("123456789012345678") == "123456...5678");
    chk("len19", mask_token("1234567890123456789") == "123456...6789");
    chk("sk", mask_token("sk-abcdefghijklmnopqrstuvwxyz") == "sk-abc...wxyz");

    if (g_fail) { std::cout << "redact: FAIL " << g_fail << "\n"; return 1; }
    std::cout << "all redact tests passed\n";
    return 0;
}
