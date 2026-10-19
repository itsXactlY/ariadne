#pragma once
// Telegram sticker description cache (gateway/sticker_cache.py).
// Descriptions are cached keyed by Telegram's stable file_unique_id so the
// vision tool runs once per sticker image, not per send.
#include "dd/core/json.hpp"
#include <filesystem>
#include <optional>
#include <string>

namespace dd {
namespace gateway {

std::filesystem::path cache_path();

core::Json load_cache();
void save_cache(const core::Json& cache);

std::optional<core::Json> get_cached_description(const std::string& file_unique_id);

void cache_sticker_description(const std::string& file_unique_id,
                               const std::string& description,
                               const std::string& emoji = "",
                               const std::string& set_name = "");

std::string build_sticker_injection(const std::string& description,
                                    const std::string& emoji = "",
                                    const std::string& set_name = "");

std::string build_animated_sticker_injection(const std::string& emoji = "");

}  // gateway
}  // dd
