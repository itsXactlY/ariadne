// Tests for the pure delegation_output_schema helpers.
#include "dd/tools/delegation_output_schema.hpp"
#include "dd/core/json.hpp"
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

using namespace dd;
using core::Json;

static int g_fail = 0;
static void chk(bool ok, const std::string& label) {
    if (!ok) { std::printf("FAIL %s\n", label.c_str()); ++g_fail; }
}

static void chk_coerce(const std::string& cfg, bool want_ok, const std::string& want_err) {
    Json raw = core::json_parse(cfg);
    auto [sch, err] = tools::coerce_output_schema(raw);
    if (want_ok) {
        chk(sch.has_value() && !err.has_value(), cfg);
    } else {
        chk(!sch.has_value() && err.has_value() && *err == want_err, cfg);
    }
}

int main() {
    // coerce_output_schema
    {
        Json raw = core::json_parse("null");
        auto [sch, err] = tools::coerce_output_schema(raw);
        chk(!sch.has_value() && !err.has_value(), "coerce_none");
    }
    chk_coerce("\"not json\"", false, "output_schema must be a JSON Schema object, got a non-JSON string.");
    chk_coerce("\"[1,2]\"", false, "output_schema must be a JSON Schema object.");
    chk_coerce("\"{\\\"type\\\":\\\"object\\\"}\"", true, "");
    chk_coerce("{\"type\":\"object\"}", true, "");
    chk_coerce("[1,2]", false, "output_schema must be a JSON Schema object, got list.");
    chk_coerce("3", false, "output_schema must be a JSON Schema object, got int.");
    chk_coerce("3.5", false, "output_schema must be a JSON Schema object, got float.");
    chk_coerce("true", false, "output_schema must be a JSON Schema object, got bool.");

    // append_output_contract
    Json sch = core::json_parse("{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}");
    const std::string ap_empty = R"AP(OUTPUT CONTRACT (machine-validated):
Your FINAL response must be a single JSON object that validates against this JSON Schema. No prose before or after the JSON; a ```json code fence is acceptable but not required.
{
  "type": "object",
  "properties": {
    "name": {
      "type": "string"
    }
  },
  "required": [
    "name"
  ]
})AP";
    chk(tools::append_output_contract("", sch) == ap_empty, "append_empty");
    const std::string ap_ctx = "  hello\n\n" + ap_empty;
    chk(tools::append_output_contract("  hello  ", sch) == ap_ctx, "append_ctx");

    // extract_json_candidate
    chk(tools::extract_json_candidate("```json\n{\"a\":1}\n```") == "{\"a\":1}", "ex1");
    chk(tools::extract_json_candidate("Sure! Here: {\"a\": 1} done") == "{\"a\": 1}", "ex3");
    chk(tools::extract_json_candidate("[1,2,3]") == "[1,2,3]", "ex4");
    chk(tools::extract_json_candidate("no json here") == "no json here", "ex5");
    chk(tools::extract_json_candidate("```json\n[1,2]\n```") == "[1,2]", "ex6");

    // build_retry_message
    const std::string rt = "Your previous final response was rejected by the output contract validator. Validation errors:\n- err one\n- err two\n\nReply with ONLY the corrected JSON object matching the OUTPUT CONTRACT schema from your task context. No prose, no explanations.";
    chk(tools::build_retry_message({"err one", "err two"}) == rt, "retry");
    const std::string rt0 = "Your previous final response was rejected by the output contract validator. Validation errors:\n\n\nReply with ONLY the corrected JSON object matching the OUTPUT CONTRACT schema from your task context. No prose, no explanations.";
    chk(tools::build_retry_message({}) == rt0, "retry_empty");

    if (g_fail == 0) { std::printf("all delegation-schema tests passed\n"); return 0; }
    std::printf("%d failures\n", g_fail);
    return 1;
}
