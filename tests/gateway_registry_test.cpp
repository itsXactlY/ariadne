// Faithful-port test for dd::gateway::PlatformRegistry (gateway/platform_registry.py).
#include "dd/gateway/platform_registry.hpp"
#include "dd/core/json.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace dd;
using namespace dd::core;

static int failures = 0;
using namespace dd::gateway;
#define CHECK(cond) do { if (!(cond)) { ++failures; \
    std::cerr << "FAIL: " << #cond << " @ " << __LINE__ << "\n"; } } while (0)

namespace {

struct FakeAdapter : PlatformAdapter {
    std::string tag;
};

Json make_config_with_server() {
    return json_parse("{\"server\":\"irc.example\",\"port\":6667}");
}
Json make_config_no_server() {
    return json_parse("{\"port\":6667}");
}

PlatformEntry make_entry(const std::string& name, const std::string& label,
                         bool check_ok, bool require_server, bool factory_throws,
                         const std::string& source) {
    PlatformEntry e;
    e.name = name;
    e.label = label;
    e.source = source;
    e.check_fn = [check_ok]() { return check_ok; };
    if (require_server) {
        e.validate_config = [](const Json& cfg) {
            return json_get(cfg, "server") != nullptr;
        };
    }
    e.adapter_factory = [factory_throws, name](const Json&) -> std::shared_ptr<PlatformAdapter> {
        if (factory_throws) throw std::runtime_error("boom");
        auto a = std::make_shared<FakeAdapter>();
        a->tag = name;
        return a;
    };
    return e;
}

}  // anon

int main() {
    PlatformRegistry reg;

    // --- create_adapter gating ---
    reg.register_entry(make_entry("irc", "IRC", /*check_ok=*/true, /*require_server=*/true,
                                  /*factory_throws=*/false, "plugin"));

    auto ok = reg.create_adapter("irc", make_config_with_server());
    CHECK(ok != nullptr);

    auto bad_cfg = reg.create_adapter("irc", make_config_no_server());
    CHECK(bad_cfg == nullptr);  // validate_config failed

    // check_fn false -> null
    reg.register_entry(make_entry("irc2", "IRC2", /*check_ok=*/false, false, false, "plugin"));
    CHECK(reg.create_adapter("irc2", make_config_with_server()) == nullptr);

    // factory throws -> null
    reg.register_entry(make_entry("throwy", "ThrowY", true, false, /*factory_throws=*/true, "plugin"));
    CHECK(reg.create_adapter("throwy", make_config_with_server()) == nullptr);

    // unknown name -> null
    CHECK(reg.create_adapter("nope", make_config_with_server()) == nullptr);

    // --- lookup / registration ---
    const PlatformEntry* e = reg.get("irc");
    CHECK(e != nullptr);
    CHECK(e && e->label == "IRC");
    CHECK(reg.get("does_not_exist") == nullptr);

    CHECK(reg.is_registered("irc"));
    CHECK(!reg.is_registered("does_not_exist"));

    // --- unregister ---
    CHECK(reg.unregister("irc2"));
    CHECK(!reg.unregister("does_not_exist"));
    CHECK(!reg.is_registered("irc2"));

    // --- deferred loader resolution ---
    reg.register_deferred("irc3", [&reg]() {
        reg.register_entry(make_entry("irc3", "IRC3", true, false, false, "plugin"));
    });
    CHECK(reg.is_registered("irc3"));  // deferred counts as registered
    const PlatformEntry* d = reg.get("irc3");  // triggers _resolve
    CHECK(d != nullptr);
    CHECK(d && d->label == "IRC3");

    // --- source filtering + insertion order ---
    reg.register_entry(make_entry("builtin_x", "BuiltInX", true, false, false, "builtin"));
    auto all = reg.all_entries();
    // order: irc, throwy, irc3, builtin_x  (irc2 was unregistered)
    CHECK(all.size() == 4);
    CHECK(all.size() >= 4 && all[0]->name == "irc");
    CHECK(all.size() >= 4 && all[1]->name == "throwy");
    CHECK(all.size() >= 4 && all[2]->name == "irc3");
    CHECK(all.size() >= 4 && all[3]->name == "builtin_x");

    auto plugins = reg.plugin_entries();
    // builtin_x excluded (source != "plugin")
    CHECK(plugins.size() == 3);
    bool saw_builtin = false;
    for (const auto* p : plugins) if (p->source != "plugin") saw_builtin = true;
    CHECK(!saw_builtin);

    // --- re-register replaces in place, keeps position ---
    reg.register_entry(make_entry("irc", "IRC-v2", true, false, false, "plugin"));
    const PlatformEntry* e2 = reg.get("irc");
    CHECK(e2 && e2->label == "IRC-v2");
    auto all2 = reg.all_entries();
    CHECK(all2.size() == 4 && all2[0]->name == "irc");  // still first position

    if (failures == 0) {
        std::cout << "all gateway-registry tests passed\n";
        return 0;
    }
    std::cout << failures << " gateway-registry test failures\n";
    return 1;
}
