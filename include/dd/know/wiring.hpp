#pragma once
// dd/know/wiring.hpp — which sources feed the skill catalogue for this home and config.
// One definition, used by dd_tui and by the dd CLI, so what `dd find` shows is what the harness sees.
//
//   <home>/skills first, then skills.external_dirs (read-only trees, ~ expanded), then the maze's
//   catalogue when skills.maze is on and a maze is configured. skills.disabled hides names.

#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "dd/context/memory.hpp"
#include "dd/core/config.hpp"
#include "dd/core/home.hpp"
#include "dd/know/skills.hpp"

namespace dd {
namespace know {

struct SkillSetup {
    bool enabled = true;                          // skills.enabled
    bool auto_route = true;                       // skills.auto_route
    bool maze = true;                             // skills.maze
    std::vector<std::filesystem::path> roots;     // <home>/skills, then skills.external_dirs
    std::set<std::string> disabled;               // skills.disabled
    Bm25fParams bm25;                             // knowledge.rank.*
    size_t page_bytes = 4200;                     // knowledge.page_bytes: one page of a skill, ~1.2k tokens
};

SkillSetup skill_setup(const core::Config& cfg, const core::Home& home);

// Process-wide settings of the know module from the config, once at startup before any catalogue is built:
//   knowledge.stopword_languages  [en, de]  which languages' stopwords are out (replaces the default: list en/de too)
// A scalar "en, de, sv" is read as a list; an empty list switches stopwords off.
void configure(const core::Config& cfg);

// mem may be null: the catalogue is then the disk's alone.
std::shared_ptr<SkillCatalogue> make_skill_catalogue(const SkillSetup& s, std::shared_ptr<context::Memory> mem);

// "~/x" -> "$HOME/x"
std::filesystem::path expand_home(const std::string& p);

// The mazemaker MCP url from the config: mcp_servers.<any enabled http server named maze*>.url; "" if none.
std::string maze_url_of(const core::Config& cfg);

}  // namespace know
}  // namespace dd
