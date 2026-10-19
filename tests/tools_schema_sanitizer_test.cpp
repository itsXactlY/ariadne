// Ground-truth test for dd::tools::schema_sanitizer vs real Python module.
#include "dd/tools/schema_sanitizer.hpp"
#include "dd/core/json.hpp"

#include <iostream>
#include <string>

static int failures = 0;
#define CHECK(cond, label)                                              \
    do {                                                                \
        if (!(cond)) {                                                  \
            ++failures;                                                 \
            std::cout << "FAIL " << label << "\n";                      \
        } else {                                                        \
            std::cout << "ok " << label << "\n";                        \
        }                                                               \
    } while (0)

using namespace dd;
using core::Json;

static std::string dump(const core::Json& v) { return core::json_dump(v) + "\n"; }

int main() {
    // ---- Case A: rich tool schema -------------------------------------
    const std::string toolsA =
        R"([{"type":"function","function":{"name":"mytool","parameters":{
        "type":"object",
        "properties":{
          "user name":{"type":"string"},
          "user.name":{"type":"integer"},
          "age":{"type":["string","null"]},
          "opt":{"anyOf":[{"type":"string"},{"type":"null"}],"description":"d"},
          "ref":{"$ref":"#/$defs/Foo","default":null},
          "bare":{"additionalProperties":"object"}
        },
        "required":["user name","missing"],
        "allOf":[{"type":"object"}],
        "enum":["a/b","c"]
      }}}])";

    Json ta = core::json_parse(toolsA);

    const std::string expA_sanitize =
        R"([{"type":"function","function":{"name":"mytool","parameters":{"type":"object","properties":{"user_name":{"type":"string"},"user.name":{"type":"integer"},"age":{"type":"string","nullable":true},"opt":{"type":"string","nullable":true,"description":"d"},"ref":{"$ref":"#/$defs/Foo"},"bare":{"additionalProperties":{"type":"object","properties":{}}}},"required":["user_name"]}}}]
)";
    Json sa = tools::sanitize_tool_schemas(ta);
    CHECK(dump(sa) == expA_sanitize, "A_sanitize");

    const std::string expA_pattern =
        R"([{"type":"function","function":{"name":"mytool","parameters":{"type":"object","properties":{"user name":{"type":"string"},"user.name":{"type":"integer"},"age":{"type":["string","null"]},"opt":{"anyOf":[{"type":"string"},{"type":"null"}],"description":"d"},"ref":{"$ref":"#/$defs/Foo","default":null},"bare":{"additionalProperties":"object"}},"required":["user name","missing"],"allOf":[{"type":"object"}],"enum":["a/b","c"]}}}]
)";
    auto pa = tools::strip_pattern_and_format(ta);
    CHECK(pa.second == 0, "A_pattern_count");
    CHECK(dump(pa.first) == expA_pattern, "A_pattern_tools");

    const std::string expA_slash =
        R"([{"type":"function","function":{"name":"mytool","parameters":{"type":"object","properties":{"user name":{"type":"string"},"user.name":{"type":"integer"},"age":{"type":["string","null"]},"opt":{"anyOf":[{"type":"string"},{"type":"null"}],"description":"d"},"ref":{"$ref":"#/$defs/Foo","default":null},"bare":{"additionalProperties":"object"}},"required":["user name","missing"],"allOf":[{"type":"object"}]}}}]
)";
    auto sl = tools::strip_slash_enum(ta);
    CHECK(sl.second == 1, "A_slash_count");
    CHECK(dump(sl.first) == expA_slash, "A_slash_tools");

    // ---- Case B: nullable unions standalone ---------------------------
    Json sb = core::json_parse(
        R"({"anyOf":[{"type":"string"},{"type":"null"}],"description":"d","title":"T"})");
    const std::string expB_hint =
        R"({"type":"string","nullable":true,"title":"T","description":"d"}
)";
    const std::string expB_nohint =
        R"({"type":"string","title":"T","description":"d"}
)";
    CHECK(dump(tools::strip_nullable_unions(sb, true)) == expB_hint, "B_nullable");
    CHECK(dump(tools::strip_nullable_unions(sb, false)) == expB_nohint, "B_nullable_nohint");

    // ---- Case C: unrename ---------------------------------------------
    Json psC = core::json_parse(
        R"({"type":"object","properties":{"user name":{"type":"string"},"nested":{"type":"object","properties":{"a b":{"type":"string"}}}}})");
    Json argsC = core::json_parse(
        R"({"user_name":"x","nested":{"a_b":"y"},"keep":"z"})");
    const std::string expC =
        R"({"user name":"x","nested":{"a b":"y"},"keep":"z"}
)";
    CHECK(dump(tools::unrename_tool_args(psC, argsC)) == expC, "C_unrename");

    // ---- Case D: sanitize_property_key --------------------------------
    CHECK(tools::sanitize_property_key("user name") == "user_name", "D_user_name");
    CHECK(tools::sanitize_property_key("user.name") == "user.name", "D_user_dot");
    CHECK(tools::sanitize_property_key("  ") == "__", "D_spaces");
    CHECK(tools::sanitize_property_key("") == "param", "D_empty");
    CHECK(tools::sanitize_property_key(std::string(70, 'a')) ==
          std::string(64, 'a'), "D_trunc");
    CHECK(tools::sanitize_property_key("ok-key_9") == "ok-key_9", "D_ok");
    CHECK(tools::sanitize_property_key("\xE6\x94\x92\xE6\x94\x94") ==
          "__", "D_cjk");  // "日本" -> two codepoints -> "__"

    if (failures == 0) {
        std::cout << "all schema-sanitizer tests passed\n";
        return 0;
    }
    std::cout << failures << " failure(s)\n";
    return 1;
}
