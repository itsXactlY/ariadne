#include "dd/cron/schedule.hpp"
#include "dd/core/json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>
#include <chrono>

namespace dd {
namespace cron {

using core::Json;

DT now() {
    long long secs = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    long long mins = secs / 60;
    if ((secs % 60 != 0) && (secs < 0)) --mins;  // floor toward -inf
    return DT{mins};
}

// ---- Civil <-> days (Howard Hinnant, proleptic Gregorian) ----
static long long days_from_civil(long long y, long long m, long long d) {
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;
    long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(long long z, long long& y, long long& m, long long& d) {
    z += 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    long long doe = z - era * 146097;
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long long mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
}

static long long floor_div(long long a, long long b) {
    long long q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

static long long dow_of(long long days) {  // 0=Sun .. 6=Sat
    long long w = (days + 4) % 7;
    if (w < 0) w += 7;
    return w;
}

static long long to_min(long long y, long long m, long long d, long long hh, long long mm) {
    return days_from_civil(y, m, d) * 1440 + hh * 60 + mm;
}

static void from_min(long long minute, long long& y, long long& m, long long& d,
                     long long& hh, long long& mm) {
    long long days = floor_div(minute, 1440);
    long long rem = minute - days * 1440;
    hh = rem / 60;
    mm = rem % 60;
    civil_from_days(days, y, m, d);
}

// ---- ISO <-> DT ----
DT parse_iso(const std::string& raw) {
    std::string s = raw;
    // trim
    size_t a = s.find_first_not_of(" \t\n\r");
    size_t b = s.find_last_not_of(" \t\n\r");
    if (a == std::string::npos) throw std::runtime_error("empty timestamp");
    s = s.substr(a, b - a + 1);

    auto need_digit = [&](size_t& pos) -> long long {
        if (pos >= s.size() || !std::isdigit((unsigned char)s[pos]))
            throw std::runtime_error("bad timestamp digit");
        return s[pos++] - '0';
    };
    auto num = [&](size_t& pos, int width) -> long long {
        long long v = 0;
        for (int i = 0; i < width; ++i) v = v * 10 + need_digit(pos);
        return v;
    };

    size_t pos = 0;
    long long y = num(pos, 4);
    if (pos < s.size() && s[pos] == '-') ++pos;
    long long mo = num(pos, 2);
    if (pos < s.size() && s[pos] == '-') ++pos;
    long long d = num(pos, 2);
    if (pos < s.size() && (s[pos] == 'T' || s[pos] == ' ')) ++pos;
    long long hh = num(pos, 2);
    if (pos < s.size() && s[pos] == ':') ++pos;
    long long mm = num(pos, 2);
    if (pos < s.size() && s[pos] == ':') {
        ++pos;
        num(pos, 2);  // seconds (minute granularity)
        // fractional seconds
        if (pos < s.size() && s[pos] == '.') {
            ++pos;
            while (pos < s.size() && std::isdigit((unsigned char)s[pos])) ++pos;
        }
    }
    long long off_min = 0;
    if (pos < s.size()) {
        if (s[pos] == 'Z' || s[pos] == 'z') {
            ++pos;
        } else if (s[pos] == '+' || s[pos] == '-') {
            int sign = (s[pos] == '-') ? -1 : 1;
            ++pos;
            long long oh = num(pos, 2);
            if (pos < s.size() && s[pos] == ':') ++pos;
            long long om = num(pos, 2);
            off_min = sign * (oh * 60 + om);
        }
    }
    long long minute = to_min(y, mo, d, hh, mm) - off_min;
    return DT{minute};
}

std::string iso_of(const DT& dt) {
    long long y, m, d, hh, mm;
    from_min(dt.minute, y, m, d, hh, mm);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04lld-%02lld-%02lldT%02lld:%02lld:%02lld+00:00",
                  y, m, d, hh, mm, 0LL);
    return std::string(buf);
}

// ---- parse_duration ----
long long parse_duration(const std::string& raw) {
    std::string s = raw;
    size_t a = s.find_first_not_of(" \t\n\r");
    size_t b = s.find_last_not_of(" \t\n\r");
    if (a == std::string::npos) throw std::runtime_error("invalid duration");
    s = s.substr(a, b - a + 1);
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);

    size_t i = 0;
    while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
    if (i == 0) throw std::runtime_error("invalid duration");
    long long value = std::stoll(s.substr(0, i));
    size_t j = i;
    while (j < s.size() && s[j] == ' ') ++j;
    std::string unit = s.substr(j);
    static const char* units[] = {"m", "min", "mins", "minute", "minutes",
                                  "h", "hr", "hrs", "hour", "hours",
                                  "d", "day", "days"};
    bool ok = false;
    for (const char* u : units) if (unit == u) { ok = true; break; }
    if (!ok) throw std::runtime_error("invalid duration");
    long long mult = (unit[0] == 'm') ? 1 : (unit[0] == 'h') ? 60 : 1440;
    return value * mult;
}

// ---- cron field parsing ----
// Returns allowed set as vector<bool> over [lo..hi]; dow maps 7->0 by caller.
static void parse_field(const std::string& text, int lo, int hi,
                        std::vector<char>& allowed, bool& restricted) {
    allowed.assign(hi - lo + 1, 0);
    restricted = (text != "*");
    size_t p = 0;
    while (true) {
        size_t comma = text.find(',', p);
        std::string tok = (comma == std::string::npos) ? text.substr(p)
                                                       : text.substr(p, comma - p);
        // step
        int step = 1;
        size_t slash = tok.find('/');
        std::string range = tok;
        if (slash != std::string::npos) {
            range = tok.substr(0, slash);
            std::string stepstr = tok.substr(slash + 1);
            if (stepstr.empty()) throw std::runtime_error("bad cron step");
            step = std::stoi(stepstr);
            if (step <= 0) throw std::runtime_error("bad cron step");
        }
        int a, b;
        if (range == "*") {
            a = lo; b = hi;
        } else {
            size_t dash = range.find('-');
            if (dash == std::string::npos) {
                a = b = std::stoi(range);
            } else {
                a = std::stoi(range.substr(0, dash));
                b = std::stoi(range.substr(dash + 1));
            }
        }
        if (a < lo || b > hi || a > b) throw std::runtime_error("bad cron range");
        for (int v = a; v <= b; v += step) allowed[v - lo] = 1;
        if (comma == std::string::npos) break;
        p = comma + 1;
    }
}

static bool all_cron_token(const std::vector<std::string>& parts) {
    if (parts.size() < 5) return false;
    for (int i = 0; i < 5; ++i) {
        const std::string& p = parts[i];
        if (p.empty()) return false;
        for (char c : p)
            if (!(std::isdigit((unsigned char)c) && c >= '0' && c <= '9') &&
                c != '*' && c != ',' && c != '-' && c != '/')
                return false;
    }
    return true;
}

static std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// ---- cron_next ----
std::optional<DT> cron_next(const std::string& expr, const DT& base) {
    std::vector<std::string> parts = split_ws(expr);
    if (parts.size() < 5) throw std::runtime_error("cron needs 5 fields");
    std::vector<char> mn, hr, dom, mon, dow;
    bool r_mn, r_hr, r_dom, r_mon, r_dow;
    parse_field(parts[0], 0, 59, mn, r_mn);
    parse_field(parts[1], 0, 23, hr, r_hr);
    parse_field(parts[2], 1, 31, dom, r_dom);
    parse_field(parts[3], 1, 12, mon, r_mon);
    // dow 0-7, map 7->0
    std::vector<char> dow7;
    bool r_dow7;
    parse_field(parts[4], 0, 7, dow7, r_dow7);
    r_dow = r_dow7;
    dow.assign(7, 0);
    for (int v = 0; v <= 7; ++v) if (dow7[v]) dow[v % 7] = 1;

    long long t = base.minute + 1;
    const long long CAP = 1000000;
    for (long long iter = 0; iter < CAP; ++iter) {
        long long y, m, d, hh, mm;
        from_min(t, y, m, d, hh, mm);
        long long days = floor_div(t, 1440);
        long long wd = dow_of(days);
        if (!mon[(int)m - 1]) {  // jump to first minute of next month
            long long ny = y, nm = m + 1;
            if (nm > 12) { nm = 1; ++ny; }
            t = to_min(ny, nm, 1, 0, 0);
            continue;
        }
        bool dom_ok = dom[(int)d - 1];
        bool dow_ok = dow[(int)wd];
        bool day_ok;
        if (r_dom && r_dow) day_ok = dom_ok || dow_ok;  // Vixie OR rule
        else day_ok = dom_ok && dow_ok;
        if (!day_ok) {  // jump to next day 00:00
            t = (days + 1) * 1440;
            continue;
        }
        if (!hr[(int)hh]) {  // jump to next hour :00
            t = (floor_div(t, 60) + 1) * 60;
            continue;
        }
        if (!mn[(int)mm]) {  // +1 minute
            ++t;
            continue;
        }
        return DT{t};
    }
    return std::nullopt;
}

// ---- parse_schedule ----
static std::string lower_of(const std::string& s) {
    std::string o = s;
    for (auto& c : o) c = (char)std::tolower((unsigned char)c);
    return o;
}

static std::string strip_of(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\n\r");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\n\r");
    return s.substr(a, b - a + 1);
}

static std::string once_at_display(const DT& dt) {
    long long y, m, d, hh, mm;
    from_min(dt.minute, y, m, d, hh, mm);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "once at %04lld-%02lld-%02lld %02lld:%02lld", y, m, d, hh, mm);
    return std::string(buf);
}

Json parse_schedule(const std::string& schedule, const DT& now) {
    std::string s = strip_of(schedule);
    std::string original = s;
    std::string sl = lower_of(s);

    if (sl.rfind("every ", 0) == 0) {
        std::string dur = strip_of(s.substr(6));
        long long minutes = parse_duration(dur);
        Json o = core::json_obj();
        core::obj_set(o, "kind", core::json_str("interval"));
        core::obj_set(o, "minutes", core::json_num((double)minutes));
        core::obj_set(o, "display", core::json_str("every " + std::to_string(minutes) + "m"));
        return o;
    }

    std::vector<std::string> parts = split_ws(s);
    if (all_cron_token(parts)) {
        // Validate the cron expression (croniter-equivalent).
        cron_next(s, now);
        Json o = core::json_obj();
        core::obj_set(o, "kind", core::json_str("cron"));
        core::obj_set(o, "expr", core::json_str(s));
        core::obj_set(o, "display", core::json_str(s));
        return o;
    }

    bool has_T = s.find('T') != std::string::npos;
    bool date_prefix = s.size() >= 10 && std::isdigit((unsigned char)s[0]) &&
                       std::isdigit((unsigned char)s[1]) && std::isdigit((unsigned char)s[2]) &&
                       std::isdigit((unsigned char)s[3]) && s[4] == '-' &&
                       std::isdigit((unsigned char)s[5]) && std::isdigit((unsigned char)s[6]) &&
                       s[7] == '-' && std::isdigit((unsigned char)s[8]) && std::isdigit((unsigned char)s[9]);
    if (has_T || date_prefix) {
        std::string iso = s;
        // Python: schedule.replace('Z','+00:00')
        std::string fixed;
        for (char c : iso) { if (c == 'Z') fixed += "+00:00"; else fixed += c; }
        DT dt = parse_iso(fixed);
        Json o = core::json_obj();
        core::obj_set(o, "kind", core::json_str("once"));
        core::obj_set(o, "run_at", core::json_str(iso_of(dt)));
        core::obj_set(o, "display", core::json_str(once_at_display(dt)));
        return o;
    }

    try {
        long long minutes = parse_duration(s);
        DT run_at{now.minute + minutes};
        Json o = core::json_obj();
        core::obj_set(o, "kind", core::json_str("once"));
        core::obj_set(o, "run_at", core::json_str(iso_of(run_at)));
        core::obj_set(o, "display", core::json_str("once in " + original));
        return o;
    } catch (const std::runtime_error&) {
        // fall through to final error
    }

    throw std::runtime_error("invalid schedule '" + original + "'");
}

// ---- grace ----
long long compute_grace_seconds(const core::Json& schedule, const DT& now) {
    const long long MIN_GRACE = 120, MAX_GRACE = 7200;
    const Json* kind = core::json_get(schedule, "kind");
    std::string k = (kind && kind->type == Json::Str) ? kind->str : "";
    if (k == "interval") {
        const Json* mins = core::json_get(schedule, "minutes");
        long long minutes = (mins && mins->type == Json::Num) ? (long long)mins->num : 1;
        long long period = minutes * 60;
        long long grace = period / 2;
        return std::max(MIN_GRACE, std::min(grace, MAX_GRACE));
    }
    if (k == "cron") {
        const Json* expr = core::json_get(schedule, "expr");
        if (expr && expr->type == Json::Str) {
            try {
                std::optional<DT> first = cron_next(expr->str, now);
                if (first) {
                    std::optional<DT> second = cron_next(expr->str, *first);
                    if (second) {
                        long long period = (second->minute - first->minute) * 60;
                        long long grace = period / 2;
                        return std::max(MIN_GRACE, std::min(grace, MAX_GRACE));
                    }
                }
            } catch (const std::runtime_error&) {
            }
        }
    }
    return MIN_GRACE;
}

// ---- recoverable oneshot ----
std::optional<std::string> recoverable_oneshot_run_at(const core::Json& schedule,
                                                             const DT& now,
                                                             const std::optional<std::string>& last_run_at) {
    const Json* kind = core::json_get(schedule, "kind");
    std::string k = (kind && kind->type == Json::Str) ? kind->str : "";
    if (k != "once") return std::nullopt;
    if (last_run_at.has_value()) return std::nullopt;
    const Json* run_at = core::json_get(schedule, "run_at");
    if (!run_at || run_at->type != Json::Str) return std::nullopt;
    DT run_dt = parse_iso(run_at->str);
    // run_at_dt >= now - 120s
    if (run_dt.minute >= now.minute - 2)  // 120s = 2 minutes
        return run_at->str;
    return std::nullopt;
}

// ---- compute_next_run ----
std::optional<std::string> compute_next_run(const core::Json& schedule,
                                            const DT& now,
                                            const std::optional<std::string>& last_run_at) {
    const Json* kind = core::json_get(schedule, "kind");
    std::string k = (kind && kind->type == Json::Str) ? kind->str : "";
    if (k == "once") {
        return recoverable_oneshot_run_at(schedule, now, last_run_at);
    }
    if (k == "interval") {
        const Json* mins = core::json_get(schedule, "minutes");
        long long minutes = (mins && mins->type == Json::Num) ? (long long)mins->num : 1;
        long long base_min = now.minute;
        if (last_run_at.has_value()) {
            DT last = parse_iso(*last_run_at);
            base_min = last.minute;
        }
        DT next{base_min + minutes};
        return iso_of(next);
    }
    if (k == "cron") {
        const Json* expr = core::json_get(schedule, "expr");
        if (!expr || expr->type != Json::Str) return std::nullopt;
        std::optional<DT> nx = cron_next(expr->str, now);
        if (!nx) return std::nullopt;
        return iso_of(*nx);
    }
    return std::nullopt;
}

}  // namespace cron
}  // namespace dd
