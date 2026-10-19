#pragma once
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dd/core/json.hpp"

namespace dd {
namespace gateway {

// Polymorphic handle for a platform adapter. Concrete adapters (Discord/Telegram/IRC/...)
// are ported separately and derive from this base; the registry only stores the factory and
// invokes it -- it never touches adapter internals. Faithful to platform_registry.py::PlatformEntry.adapter_factory
// (Python returns Any; here the handle is shared_ptr<PlatformAdapter>).
struct PlatformAdapter {
    virtual ~PlatformAdapter() = default;
};

// Metadata + factory for a single platform adapter. Faithful port of
// gateway/platform_registry.py::PlatformEntry. Callable fields the registry never invokes
// (is_connected/setup_fn/env_enablement_fn/apply_yaml_config_fn/standalone_sender_fn) are
// stored as metadata with faithful-ish signatures; empty std::function == Python None.
struct PlatformEntry {
    std::string name;
    std::string label;
    std::function<std::shared_ptr<PlatformAdapter>(const dd::core::Json&)> adapter_factory;
    std::function<bool()> check_fn;
    std::function<bool(const dd::core::Json&)> validate_config;   // empty == None
    std::function<bool(const dd::core::Json&)> is_connected;       // empty == None
    std::vector<std::string> required_env;
    std::string install_hint;
    std::function<void()> setup_fn;                                 // empty == None
    std::string source = "plugin";
    std::string plugin_name;
    std::string allowed_users_env;
    std::string allow_all_env;
    int max_message_length = 0;
    bool pii_safe = false;
    std::string emoji = "\xF0\x9F\x94\x8C";   // U+1F48C 🔌
    bool allow_update_command = true;
    std::string platform_hint;
    std::function<std::optional<dd::core::Json>()> env_enablement_fn;   // empty == None
    std::function<std::optional<dd::core::Json>(const dd::core::Json&, const dd::core::Json&)> apply_yaml_config_fn;  // empty == None
    std::string cron_deliver_env_var;
    std::function<std::optional<dd::core::Json>(const dd::core::Json&)> standalone_sender_fn;  // empty == None
};

// Central registry of platform adapters. Faithful port of
// gateway/platform_registry.py::PlatformRegistry. Insertion order of entries is preserved
// (Python dict order feeds all_entries()/plugin_entries() display), tracked via order_.
class PlatformRegistry {
public:
    void register_deferred(const std::string& name, std::function<void()> loader);
    void register_entry(const PlatformEntry& entry);
    bool unregister(const std::string& name);
    const PlatformEntry* get(const std::string& name);
    std::vector<const PlatformEntry*> all_entries();
    std::vector<const PlatformEntry*> plugin_entries();
    bool is_registered(const std::string& name) const;
    std::shared_ptr<PlatformAdapter> create_adapter(const std::string& name,
                                                    const dd::core::Json& config);

private:
    std::map<std::string, PlatformEntry> entries_;
    std::vector<std::string> order_;   // insertion order of names
    std::map<std::string, std::function<void()>> deferred_;

    void _resolve(const std::string& name);
    void _resolve_all();
};

// Module-level singleton (platform_registry = PlatformRegistry()).
PlatformRegistry& platform_registry();

}  // namespace gateway
}  // namespace dd
