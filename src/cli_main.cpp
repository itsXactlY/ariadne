// dd — daedalus-cpp harness CLI (skills subsystem).
// Subcommands: list | view <name> [file] | resolve <skill://...>
#include "dd/skills/scanner.hpp"
#include "dd/skills/registry.hpp"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace dd::skills;

static std::filesystem::path skills_root() {
    const char* env = std::getenv("DAEDALUS_SKILLS_DIR");
    if (env && *env) return env;
    return std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : "") /
           ".daedalus" / "skills";
}

static Registry build_registry() {
    Scanner sc;
    sc.add_root(skills_root());
    sc.set_platform("linux");
    Registry reg;
    reg.build(sc.scan());
    return reg;
}

static void cmd_list(int argc, char** argv) {
    std::string category;
    int limit = 20;
    bool names_only = false;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--category" && i + 1 < argc) category = argv[++i];
        else if (a == "--limit" && i + 1 < argc) limit = std::stoi(argv[++i]);
        else if (a == "--names-only") names_only = true;
    }
    Registry reg = build_registry();
    auto counts = reg.category_counts();
    std::cout << "total=" << reg.all().size() << " categories=" << counts.size() << "\n";
    for (const auto& [c, n] : counts) std::cout << "  " << c << ": " << n << "\n";

    std::vector<const Skill*> sel;
    for (const auto& s : reg.all())
        if (category.empty() || s.category == category) sel.push_back(&s);
    if (limit > 0 && (int)sel.size() > limit) sel.resize(limit);
    for (const auto* s : sel) {
        std::cout << "  [" << (s->category.empty() ? "-" : s->category) << "] "
                  << s->name;
        if (!names_only) std::cout << " — " << s->description;
        std::cout << "\n";
    }
}

static void cmd_view(int argc, char** argv) {
    if (argc < 3) { std::cerr << "usage: dd view <name> [file]\n"; return; }
    Registry reg = build_registry();
    std::string file = (argc > 3) ? argv[3] : "";
    auto content = reg.view(argv[2], file);
    if (content.empty()) { std::cerr << "skill '" << argv[2] << "' not found\n"; return; }
    std::cout << content;
}

static void cmd_resolve(int argc, char** argv) {
    if (argc < 3) { std::cerr << "usage: dd resolve <skill://...>\n"; return; }
    Registry reg = build_registry();
    auto content = reg.resolve(argv[2]);
    if (content.empty()) { std::cerr << "uri '" << argv[2] << "' not resolved\n"; return; }
    std::cout << content;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "usage: dd <list|view|resolve> ...\n"; return 1; }
    std::string cmd = argv[1];
    if (cmd == "list") cmd_list(argc, argv);
    else if (cmd == "view") cmd_view(argc, argv);
    else if (cmd == "resolve") cmd_resolve(argc, argv);
    else { std::cerr << "unknown command: " << cmd << "\n"; return 1; }
    return 0;
}
