#include "dd/cron/schedule.hpp"
#include "dd/core/json.hpp"

#include <iostream>
#include <optional>
#include <string>

using namespace dd;
using namespace dd::core;
using namespace dd::cron;

static int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            ++failures;                                                   \
            std::cerr << "FAIL: " << #cond << " @ " << __LINE__ << "\n";  \
        }                                                                 \
    } while (0)
#define CHECK_THROWS(e)                                                   \
    do {                                                                  \
        bool _t = false;                                                  \
        try {                                                             \
            (e);                                                          \
        } catch (const std::runtime_error&) {                             \
            _t = true;                                                    \
        }                                                                 \
        if (!_t) {                                                        \
            ++failures;                                                   \
            std::cerr << "FAIL throws: " << #e << " @ " << __LINE__ << "\n"; \
        }                                                                 \
    } while (0)

static std::string get_str(const Json& o, const std::string& k) {
    const Json* v = json_get(o, k);
    return (v && v->type == Json::Str) ? v->str : std::string("<none>");
}
static long long get_num(const Json& o, const std::string& k) {
    const Json* v = json_get(o, k);
    return (v && v->type == Json::Num) ? (long long)v->num : -999999;
}

int main() {
    DT now = parse_iso("2026-02-03T10:00:00");  // Tuesday 10:00 UTC

    // ---- parse_duration ----
    CHECK(parse_duration("30m") == 30);
    CHECK(parse_duration("2h") == 120);
    CHECK(parse_duration("1d") == 1440);
    CHECK(parse_duration("45 minutes") == 45);
    CHECK(parse_duration("  2 hrs ") == 120);
    CHECK_THROWS(parse_duration("45"));
    CHECK_THROWS(parse_duration("2h30m"));
    CHECK_THROWS(parse_duration(""));

    // ---- parse_schedule ----
    Json iv = parse_schedule("every 30m", now);
    CHECK(get_str(iv, "kind") == "interval");
    CHECK(get_num(iv, "minutes") == 30);
    CHECK(get_str(iv, "display") == "every 30m");

    Json cr = parse_schedule("0 9 * * *", now);
    CHECK(get_str(cr, "kind") == "cron");
    CHECK(get_str(cr, "expr") == "0 9 * * *");

    Json once = parse_schedule("2026-02-03T14:00", now);
    CHECK(get_str(once, "kind") == "once");
    CHECK(get_str(once, "run_at") == "2026-02-03T14:00:00+00:00");
    CHECK(get_str(once, "display") == "once at 2026-02-03 14:00");

    Json rel = parse_schedule("30m", now);
    CHECK(get_str(rel, "kind") == "once");
    CHECK(get_str(rel, "run_at") == "2026-02-03T10:30:00+00:00");
    CHECK(get_str(rel, "display") == "once in 30m");

    CHECK_THROWS(parse_schedule("not a schedule", now));

    // ---- cron_next ----
    {
        std::optional<DT> n = cron_next("0 9 * * *", now);
        CHECK(n.has_value());
        CHECK(n && iso_of(*n) == "2026-02-04T09:00:00+00:00");
    }
    {
        std::optional<DT> n = cron_next("*/15 * * * *", now);
        CHECK(n && iso_of(*n) == "2026-02-03T10:15:00+00:00");
    }
    {
        // next Monday (2026-02-03 is Tue; next Mon = 02-09)
        std::optional<DT> n = cron_next("0 0 * * 1", now);
        CHECK(n && iso_of(*n) == "2026-02-09T00:00:00+00:00");
    }
    {
        // dow 7 == Sunday; next Sunday after Tue 02-03 is 02-08
        std::optional<DT> n = cron_next("0 0 * * 7", now);
        CHECK(n && iso_of(*n) == "2026-02-08T00:00:00+00:00");
    }
    {
        std::optional<DT> n = cron_next("0 0 1 * *", now);
        CHECK(n && iso_of(*n) == "2026-03-01T00:00:00+00:00");
    }
    {
        // Vixie OR rule: dom=1 OR dow=Fri(5). Fri after Tue 02-03 is 02-06.
        std::optional<DT> n = cron_next("0 0 1 * 5", now);
        CHECK(n && iso_of(*n) == "2026-02-06T00:00:00+00:00");
    }
    CHECK_THROWS(cron_next("bad expr", now));

    // ---- compute_next_run ----
    {
        std::optional<std::string> r =
            compute_next_run(iv, now, std::optional<std::string>("2026-02-03T10:00:00+00:00"));
        CHECK(r && *r == "2026-02-03T10:30:00+00:00");
    }
    {
        std::optional<std::string> r = compute_next_run(iv, now, std::nullopt);
        CHECK(r && *r == "2026-02-03T10:30:00+00:00");
    }
    {
        std::optional<std::string> r = compute_next_run(cr, now, std::nullopt);
        CHECK(r && *r == "2026-02-04T09:00:00+00:00");
    }

    // ---- grace ----
    CHECK(compute_grace_seconds(parse_schedule("every 10m", now), now) == 300);
    CHECK(compute_grace_seconds(parse_schedule("every 1m", now), now) == 120);   // clamp low
    CHECK(compute_grace_seconds(parse_schedule("every 600m", now), now) == 7200);  // clamp high
    CHECK(compute_grace_seconds(cr, now) == 7200);  // daily cron period 86400s -> clamp high

    // ---- recoverable oneshot ----
    {
        Json s = core::json_obj();
        obj_set(s, "kind", core::json_str("once"));
        obj_set(s, "run_at", core::json_str("2026-02-03T10:01:00+00:00"));  // 1 min ahead
        std::optional<std::string> r = recoverable_oneshot_run_at(s, now, std::nullopt);
        CHECK(r && *r == "2026-02-03T10:01:00+00:00");
    }
    {
        Json s = core::json_obj();
        obj_set(s, "kind", core::json_str("once"));
        obj_set(s, "run_at", core::json_str("2026-02-03T09:55:00+00:00"));  // 5 min behind
        std::optional<std::string> r = recoverable_oneshot_run_at(s, now, std::nullopt);
        CHECK(!r.has_value());
    }
    {
        Json s = core::json_obj();
        obj_set(s, "kind", core::json_str("once"));
        obj_set(s, "run_at", core::json_str("2026-02-03T10:01:00+00:00"));
        std::optional<std::string> r =
            recoverable_oneshot_run_at(s, now, std::optional<std::string>("2026-02-03T09:00:00+00:00"));
        CHECK(!r.has_value());  // already ran
    }

    if (failures == 0)
        std::cout << "all cron tests passed\n";
    else
        std::cout << failures << " cron test failures\n";
    return failures == 0 ? 0 : 1;
}
