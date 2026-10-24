// Ground-truth tests for tools/ansi_strip.py port.
#include "dd/tools/ansi_strip.hpp"
#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>

using namespace dd::tools;

// Build a UTF-8 string from a list of codepoints.
static std::string mk(std::initializer_list<uint32_t> cps) {
    std::string s;
    for (uint32_t c : cps) {
        if (c < 0x80) s.push_back(static_cast<char>(c));
        else if (c < 0x800) { s.push_back(static_cast<char>(0xC0|(c>>6))); s.push_back(static_cast<char>(0x80|(c&0x3F))); }
        else if (c < 0x10000) { s.push_back(static_cast<char>(0xE0|(c>>12))); s.push_back(static_cast<char>(0x80|((c>>6)&0x3F))); s.push_back(static_cast<char>(0x80|(c&0x3F))); }
        else { s.push_back(static_cast<char>(0xF0|(c>>18))); s.push_back(static_cast<char>(0x80|((c>>12)&0x3F))); s.push_back(static_cast<char>(0x80|((c>>6)&0x3F))); s.push_back(static_cast<char>(0x80|(c&0x3F))); }
    }
    return s;
}

static int g_fail = 0;
static void chk(bool ok, const std::string& label) { if (!ok) { ++g_fail; std::printf("FAIL %s\n", label.c_str()); } }

int main() {
    const uint32_t ESC = 0x1B, BEL = 0x07, BSL = 0x5C;

    // ---- strip_ansi ----
    chk(strip_ansi("plain text") == "plain text", "s_plain");
    chk(strip_ansi(mk({ESC,'[','3','1','m','r','e','d',ESC,'[','0','m'})) == "red", "s_csi_red");
    chk(strip_ansi(mk({ESC,']','0',';','t','i','t','l','e',BEL})) == "", "s_osc_bel");
    chk(strip_ansi(mk({ESC,']','0',';','t','i','t','l','e',ESC,BSL})) == "", "s_osc_st");
    chk(strip_ansi(mk({ESC,'P','f','o','o',ESC,BSL})) == "", "s_dcs");
    chk(strip_ansi(mk({ESC,'X','f','o','o',ESC,BSL})) == "", "s_sos");
    chk(strip_ansi(mk({ESC,'^','f','o','o',ESC,BSL})) == "", "s_pm");
    chk(strip_ansi(mk({ESC,'_','f','o','o',ESC,BSL})) == "", "s_apc");
    chk(strip_ansi(mk({ESC,'[','?','2','5','l'})) == "", "s_csi_priv");
    chk(strip_ansi(mk({ESC,'[','1',';','3','1',';','4','2','m'})) == "", "s_csi_multi");
    chk(strip_ansi(mk({ESC,'[','1','0','0','0','n'})) == "", "s_csi_n");
    chk(strip_ansi(mk({ESC,'[','1','0','0','0','m'})) == "", "s_csi_m");
    chk(strip_ansi(mk({ESC,'7'})) == "", "s_fe_7");
    chk(strip_ansi(mk({ESC,'M'})) == "", "s_fe_M");
    chk(strip_ansi(mk({ESC,'A'})) == "", "s_fe_A");
    chk(strip_ansi(mk({ESC,' ','A'})) == "", "s_nf1");
    chk(strip_ansi(mk({ESC,' ',' ','A'})) == "", "s_nf2");
    chk(strip_ansi(mk({0x9B,'3','1','m'})) == "", "s_c1csi");
    chk(strip_ansi(mk({0x9D,'f','o','o',BEL})) == "", "s_c1osc_bel");
    chk(strip_ansi(mk({0x9D,'f','o','o',0x9C})) == "", "s_c1osc_st");
    chk(strip_ansi(mk({0x80})) == "", "s_bare_c1_80");
    chk(strip_ansi(mk({0x9F})) == "", "s_bare_c1_9f");
    chk(strip_ansi(mk({ESC})) == mk({ESC}), "s_lone_esc");
    chk(strip_ansi(mk({ESC,'['})) == "", "s_esc_bracket");
    chk(strip_ansi(mk({ESC,'[','3','1'})) == "31", "s_esc_bracket_31");
    chk(strip_ansi("a" + mk({ESC,'[','3','1','m'}) + "b" + mk({ESC,'[','0','m'}) + "c") == "abc", "s_interleaved");
    chk(strip_ansi(mk({ESC,'[','3','1','m',ESC,'[','0','m'})) == "", "s_two_csi");

    // ---- sanitize_display_text ----
    chk(sanitize_display_text("plain text") == "plain text", "d_plain");
    chk(sanitize_display_text(mk({ESC,'[','3','1','m','r','e','d',ESC,'[','0','m'})) == "red", "d_csi_red");
    chk(sanitize_display_text(mk({ESC})) == "", "d_lone_esc");
    chk(sanitize_display_text(mk({ESC,'['})) == "", "d_esc_bracket");
    chk(sanitize_display_text(mk({ESC,'[','3','1'})) == "31", "d_esc_bracket_31");
    chk(sanitize_display_text(mk({0x80})) == "", "d_bare_c1_80");
    chk(sanitize_display_text(mk({0x9F})) == "", "d_bare_c1_9f");
    chk(sanitize_display_text(mk({0x61,0x00,0x62})) == "ab", "d_nul");
    chk(sanitize_display_text("a\x08" "b") == "ab", "d_bs");
    chk(sanitize_display_text("a\x0b" "b") == "ab", "d_vt");
    chk(sanitize_display_text("a\x0c" "b") == "ab", "d_ff");
    chk(sanitize_display_text("a\x0e" "b") == "ab", "d_so");
    chk(sanitize_display_text("a\x1f" "b") == "ab", "d_us");
    chk(sanitize_display_text("a\x7f" "b") == "ab", "d_del");
    chk(sanitize_display_text("a\r\nb") == "a\nb", "d_crlf");
    chk(sanitize_display_text("a\rb") == "a\nb", "d_cr");
    chk(sanitize_display_text("a\tb") == "a\tb", "d_tab_preserved");
    chk(sanitize_display_text("a" + mk({0x80}) + "b") == "ab", "d_c1_mid");
    chk(sanitize_display_text("a" + mk({0x9F}) + "b") == "ab", "d_c1_mid_9f");

    // ---- supplementary-plane characters survive the slow path byte for byte ----
    {
        const std::string emoji = mk({0x1F600}), top = mk({0x10FFFF}), low = mk({0x10000}), mid = mk({0x20AC});
        chk(emoji == "\xF0\x9F\x98\x80", "mk emoji bytes");
        chk(sanitize_display_text("ok " + emoji + " done") == "ok " + emoji + " done", "p_emoji_fast");
        chk(sanitize_display_text(mk({ESC,'[','3','1','m'}) + "red" + mk({ESC,'[','0','m'}) + " " + emoji + " tail\r\n") == "red " + emoji + " tail\n",
            "p_emoji_ansi_crlf");
        chk(sanitize_display_text("a\r" + low + top + mid + emoji) == "a\n" + low + top + mid + emoji, "p_planes_cr");
        chk(strip_ansi(mk({ESC,'[','3','1','m'}) + emoji) == emoji, "p_strip_emoji");
        chk(strip_ansi(mk({0x9B,'3','1','m'}) + low + top) == low + top, "p_strip_planes");
    }

    // ---- a byte that is not a well-formed sequence is one character of its own, never a smuggled one ----
    {
        // overlong '/', surrogate, past U+10FFFF: each lead byte falls back to its Latin-1 value
        chk(sanitize_display_text("\rx\xC0\xAF") == "\nx" + mk({0xC0, 0xAF}), "i_overlong_slash");
        chk(sanitize_display_text("\r\xED\xA0\x80").find("\xED\xA0\x80") == std::string::npos, "i_surrogate_not_passed");
        chk(sanitize_display_text("\r\xF4\x90\x80\x80").find("\xF4\x90") == std::string::npos, "i_past_10ffff");
        chk(sanitize_display_text("\rx\xE0\x80\xAF") == "\nx" + mk({0xE0}) + mk({0xAF}), "i_overlong_3byte_slash");
        // an overlong ESC must not turn into a real one
        chk(strip_ansi("\xC1\x9B[31mx") == "\xC1\x9B[31mx" || strip_ansi("\xC1\x9B[31mx").find('\x1b') == std::string::npos, "i_overlong_esc");
    }

    if (g_fail == 0) { std::printf("all ansi-strip tests passed\n"); return 0; }
    std::printf("%d failures\n", g_fail);
    return 1;
}
