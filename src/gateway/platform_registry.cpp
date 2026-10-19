#include "dd/gateway/platform_registry.hpp"
#include "dd/core/json.hpp"

#include <algorithm>
#include <iostream>

namespace dd {
namespace gateway {

void PlatformRegistry::register_deferred(const std::string& name, std::function<void()> loader) {
    if (entries_.count(name)) return;
    deferred_[name] = std::move(loader);
}

void PlatformRegistry::_resolve(const std::string& name) {
    auto it = deferred_.find(name);
    if (it == deferred_.end()) return;
    std::function<void()> loader = it->second;
    deferred_.erase(it);
    try {
        loader();
    } catch (const std::exception& e) {
        std::cerr << "[warn] Deferred load of platform '" << name << "' failed: " << e.what() << "\n";
    }
}

void PlatformRegistry::_resolve_all() {
    if (deferred_.empty()) return;
    std::vector<std::string> names;
    names.reserve(deferred_.size());
    for (const auto& kv : deferred_) names.push_back(kv.first);
    for (const auto& n : names) _resolve(n);
}

void PlatformRegistry::register_entry(const PlatformEntry& entry) {
    deferred_.erase(entry.name);
    auto it = entries_.find(entry.name);
    if (it != entries_.end()) {
        std::cerr << "[info] Platform '" << entry.name << "' re-registered (was "
                  << it->second.source << ", now " << entry.source << ")\n";
        it->second = entry;
    } else {
        order_.push_back(entry.name);
        entries_[entry.name] = entry;
    }
    std::cerr << "[debug] Registered platform adapter: " << entry.name << " ("
              << entry.source << ")\n";
}

bool PlatformRegistry::unregister(const std::string& name) {
    deferred_.erase(name);
    auto it = entries_.find(name);
    if (it == entries_.end()) return false;
    entries_.erase(it);
    order_.erase(std::remove(order_.begin(), order_.end(), name), order_.end());
    return true;
}

const PlatformEntry* PlatformRegistry::get(const std::string& name) {
    if (!entries_.count(name)) _resolve(name);
    auto it = entries_.find(name);
    if (it == entries_.end()) return nullptr;
    return &it->second;
}

std::vector<const PlatformEntry*> PlatformRegistry::all_entries() {
    _resolve_all();
    std::vector<const PlatformEntry*> out;
    for (const auto& n : order_) {
        auto it = entries_.find(n);
        if (it != entries_.end()) out.push_back(&it->second);
    }
    return out;
}

std::vector<const PlatformEntry*> PlatformRegistry::plugin_entries() {
    _resolve_all();
    std::vector<const PlatformEntry*> out;
    for (const auto& n : order_) {
        auto it = entries_.find(n);
        if (it != entries_.end() && it->second.source == "plugin") out.push_back(&it->second);
    }
    return out;
}

bool PlatformRegistry::is_registered(const std::string& name) const {
    return entries_.count(name) || deferred_.count(name);
}

std::shared_ptr<PlatformAdapter> PlatformRegistry::create_adapter(const std::string& name,
                                                                  const dd::core::Json& config) {
    if (!entries_.count(name)) _resolve(name);
    auto it = entries_.find(name);
    if (it == entries_.end()) return nullptr;
    const PlatformEntry& entry = it->second;

    if (!entry.check_fn()) {
        std::string hint = entry.install_hint.empty() ? std::string()
                                                       : (" (" + entry.install_hint + ")");
        std::cerr << "[warn] Platform '" << entry.label << "' requirements not met" << hint << "\n";
        return nullptr;
    }

    if (entry.validate_config) {
        try {
            if (!entry.validate_config(config)) {
                std::cerr << "[warn] Platform '" << entry.label << "' config validation failed\n";
                return nullptr;
            }
        } catch (const std::exception& e) {
            std::cerr << "[warn] Platform '" << entry.label << "' config validation error: "
                      << e.what() << "\n";
            return nullptr;
        }
    }

    try {
        return entry.adapter_factory(config);
    } catch (const std::exception& e) {
        std::cerr << "[error] Failed to create adapter for platform '" << entry.label << "': "
                  << e.what() << "\n";
        return nullptr;
    }
}

PlatformRegistry& platform_registry() {
    static PlatformRegistry reg;
    return reg;
}

}  // namespace gateway
}  // namespace dd
