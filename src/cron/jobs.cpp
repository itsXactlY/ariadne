#include "dd/cron/jobs.hpp"
#include "dd/cron/schedule.hpp"
#include "dd/core/home.hpp"
#include "dd/core/json.hpp"

#include <algorithm>
#include <fcntl.h>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace dd {
namespace cron {

using namespace core;

// ---- module constants (recomputed from the live home each call) ----
static std::filesystem::path g_cron_dir;
static std::filesystem::path g_jobs_file;
static std::filesystem::path g_output_dir;

static void refresh_paths() {
    std::filesystem::path home = core::get_daedalus_home();
    g_cron_dir = home / "cron";
    g_jobs_file = g_cron_dir / "jobs.json";
    g_output_dir = g_cron_dir / "output";
}

std::filesystem::path cron_dir() { refresh_paths(); return g_cron_dir; }
std::filesystem::path jobs_file() { refresh_paths(); return g_jobs_file; }
std::filesystem::path output_dir() { refresh_paths(); return g_output_dir; }

// ---- helpers ----
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\n\r");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\n\r");
    return s.substr(a, b - a + 1);
}

static std::string rstrip_slash(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

static std::string json_str_of(const Json* v) {
    if (!v) return "";
    if (v->type == Json::Str) return v->str;
    return "";
}

static Json opt_str_to_json(const std::optional<std::string>& s) {
    if (!s.has_value()) return Json::make_null();
    return json_str(*s);
}

// ---- skill normalization ----
static std::vector<std::string> normalize_skill_list(const Json* skill, const Json* skills) {
    std::vector<Json> raw;
    if (!skills || skills->type == Json::Null) {
        if (skill && skill->type == Json::Str && !skill->str.empty()) raw.push_back(*skill);
    } else if (skills->type == Json::Str) {
        raw.push_back(*skills);
    } else if (skills->type == Json::Arr) {
        for (const auto& e : skills->arr) raw.push_back(*e);
    }
    std::vector<std::string> out;
    for (const auto& item : raw) {
        std::string text = trim(json_str_of(&item));
        if (!text.empty() &&
            std::find(out.begin(), out.end(), text) == out.end())
            out.push_back(text);
    }
    return out;
}

static Json apply_skill_fields(const Json& job) {
    Json out = json_deep_copy(job);
    const Json* sk = json_get(out, "skill");
    const Json* sks = json_get(out, "skills");
    std::vector<std::string> norm = normalize_skill_list(sk, sks);
    Json arr = json_arr();
    for (const auto& s : norm) arr_push(arr, json_str(s));
    obj_set(out, "skills", arr);
    if (!norm.empty())
        obj_set(out, "skill", json_str(norm[0]));
    else
        obj_set(out, "skill", Json::make_null());
    return out;
}

// merge(a,b): deep copy of a, then overwrite keys present in b.
static Json merge_json(const Json& a, const Json& b) {
    Json out = json_deep_copy(a);
    if (b.type == Json::Obj) {
        for (const auto& kv : b.obj) obj_set(out, kv.first, *kv.second);
    }
    return out;
}

// ---- time ----
static std::string now_iso() { return iso_of(now()); }

static std::optional<std::string> next_run(const Json& sched,
                                           const std::optional<std::string>& last) {
    return compute_next_run(sched, now(), last);
}

// ---- secure perms ----
static void secure_dir(const std::filesystem::path& p) {
    ::chmod(p.c_str(), 0700);  // ignore errors
}
static void secure_file(const std::filesystem::path& p) {
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) ::chmod(p.c_str(), 0600);
}

void ensure_dirs() {
    refresh_paths();
    std::error_code ec;
    std::filesystem::create_directories(g_cron_dir, ec);
    std::filesystem::create_directories(g_output_dir, ec);
    secure_dir(g_cron_dir);
    secure_dir(g_output_dir);
}

// ---- file read ----
static std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + p.string());
    std::string s((std::istreambuf_iterator<char>(in)),
                  std::istreambuf_iterator<char>());
    return s;
}

// ---- storage ----
std::vector<Json> load_jobs() {
    ensure_dirs();
    std::error_code ec;
    if (!std::filesystem::exists(g_jobs_file, ec)) return {};
    std::string text;
    try {
        text = read_file(g_jobs_file);
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to read cron database");
    }
    Json data;
    try {
        data = json_parse(text);
    } catch (const std::exception&) {
        // auto-repair: strip control chars, re-parse
        std::string clean;
        for (unsigned char c : text)
            if (c >= 0x20 || c == '\t' || c == '\n' || c == '\r') clean.push_back((char)c);
        try {
            data = json_parse(clean);
        } catch (const std::exception& e) {
            throw std::runtime_error("Cron database corrupted and unrepairable");
        }
        std::vector<Json> jobs;
        const Json* jv = json_get(data, "jobs");
        if (jv && jv->type == Json::Arr)
            for (const auto& e : jv->arr) jobs.push_back(*e);
        if (!jobs.empty()) save_jobs(jobs);
        return jobs;
    }
    std::vector<Json> jobs;
    const Json* jv = json_get(data, "jobs");
    if (jv && jv->type == Json::Arr)
        for (const auto& e : jv->arr) jobs.push_back(*e);
    return jobs;
}

void save_jobs(const std::vector<Json>& jobs) {
    ensure_dirs();
    Json root = json_obj();
    Json arr = json_arr();
    for (const auto& j : jobs) arr_push(arr, j);
    obj_set(root, "jobs", arr);
    obj_set(root, "updated_at", json_str(now_iso()));

    std::string tmpname = (g_jobs_file.parent_path() / ".jobs_XXXXXX.tmp").string();
    std::vector<char> buf(tmpname.begin(), tmpname.end());
    buf.push_back('\0');
    int fd = ::mkstemps(buf.data(), 4);  // 4 = len(".tmp")
    if (fd < 0) throw std::runtime_error("mkstemp failed");
    std::string tmp_path(buf.data());
    try {
        {
            std::ofstream out(tmp_path, std::ios::binary);
            out << json_dump(root);
            out.flush();
            int ffd = ::open(tmp_path.c_str(), O_RDONLY);
            if (ffd >= 0) { ::fsync(ffd); ::close(ffd); }
        }
        std::error_code ec;
        std::filesystem::rename(tmp_path, g_jobs_file, ec);
        if (ec) { ::unlink(tmp_path.c_str()); throw std::runtime_error("rename failed"); }
        secure_file(g_jobs_file);
    } catch (...) {
        ::unlink(tmp_path.c_str());
        throw;
    }
}

// ---- create_job ----
core::Json create_job(const CreateJobParams& p) {
    Json parsed = parse_schedule(p.schedule, now());

    long long repeat_val = -1;  // -1 == forever
    {
        long long rv = p.repeat.value_or(-1);
        if (rv > 0) repeat_val = rv;
    }
    const Json* kind = json_get(parsed, "kind");
    bool is_once = kind && kind->type == Json::Str && kind->str == "once";
    if (is_once && repeat_val == -1) repeat_val = 1;

    std::string deliver;
    if (p.deliver.has_value()) {
        deliver = *p.deliver;
    } else {
        bool has_origin = p.origin.has_value() &&
                          !(p.origin->type == Json::Null);
        deliver = has_origin ? "origin" : "local";
    }

    // uuid4().hex[:12]
    static thread_local std::mt19937_64 rng([] { std::random_device rd; std::seed_seq seed{rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd()}; return std::mt19937_64(seed); }());
    std::uniform_int_distribution<int> hexd(0, 15);
    std::string job_id;
    for (int i = 0; i < 12; ++i) {
        int v = hexd(rng);
        job_id.push_back(v < 10 ? char('0' + v) : char('a' + (v - 10)));
    }

    Json skill_val = p.skill.has_value() ? opt_str_to_json(p.skill) : Json::make_null();
    std::vector<std::string> norm_skills;
    if (p.skills.has_value()) {
        Json sk = json_arr();
        for (const auto& s : *p.skills) arr_push(sk, json_str(s));
        norm_skills = normalize_skill_list(&skill_val, &sk);
    } else {
        Json sk_null = Json::make_null();
        norm_skills = normalize_skill_list(&skill_val, &sk_null);
    }

    auto norm_opt = [](const std::optional<std::string>& s, bool slash)
        -> std::optional<std::string> {
        if (!s.has_value()) return std::nullopt;
        std::string t = trim(*s);
        if (slash) t = rstrip_slash(t);
        if (t.empty()) return std::nullopt;
        return t;
    };

    std::optional<std::string> nmodel = norm_opt(p.model, false);
    std::optional<std::string> nprovider = norm_opt(p.provider, false);
    std::optional<std::string> nbase = norm_opt(p.base_url, true);
    std::optional<std::string> nscript = norm_opt(p.script, false);

    std::string label_source;
    if (!p.prompt.empty()) label_source = p.prompt;
    else if (!norm_skills.empty()) label_source = norm_skills[0];
    else label_source = "cron job";

    std::string name_val;
    if (p.name.has_value() && !p.name->empty()) {
        name_val = *p.name;
    } else {
        std::string t = label_source.substr(0, std::min<size_t>(50, label_source.size()));
        name_val = trim(t);
    }

    Json job = json_obj();
    obj_set(job, "id", json_str(job_id));
    obj_set(job, "name", json_str(name_val));
    obj_set(job, "prompt", json_str(p.prompt));
    Json skills_arr = json_arr();
    for (const auto& s : norm_skills) arr_push(skills_arr, json_str(s));
    obj_set(job, "skills", skills_arr);
    if (!norm_skills.empty()) obj_set(job, "skill", json_str(norm_skills[0]));
    else obj_set(job, "skill", Json::make_null());
    obj_set(job, "model", opt_str_to_json(nmodel));
    obj_set(job, "provider", opt_str_to_json(nprovider));
    obj_set(job, "base_url", opt_str_to_json(nbase));
    obj_set(job, "script", opt_str_to_json(nscript));
    obj_set(job, "schedule", parsed);
    const Json* disp = json_get(parsed, "display");
    obj_set(job, "schedule_display", disp ? *disp : json_str(p.schedule));
    Json repeat_obj = json_obj();
    if (repeat_val != -1) obj_set(repeat_obj, "times", json_num((double)repeat_val));
    else obj_set(repeat_obj, "times", Json::make_null());
    obj_set(repeat_obj, "completed", json_num(0));
    obj_set(job, "repeat", repeat_obj);
    obj_set(job, "enabled", json_bool(true));
    obj_set(job, "state", json_str("scheduled"));
    obj_set(job, "paused_at", Json::make_null());
    obj_set(job, "paused_reason", Json::make_null());
    obj_set(job, "created_at", json_str(now_iso()));
    auto nr = next_run(parsed, std::nullopt);
    obj_set(job, "next_run_at", nr ? opt_str_to_json(nr) : Json::make_null());
    obj_set(job, "last_run_at", Json::make_null());
    obj_set(job, "last_status", Json::make_null());
    obj_set(job, "last_error", Json::make_null());
    obj_set(job, "last_delivery_error", Json::make_null());
    obj_set(job, "deliver", json_str(deliver));
    obj_set(job, "origin", p.origin.has_value() ? *p.origin : Json::make_null());

    std::vector<Json> jobs = load_jobs();
    jobs.push_back(job);
    save_jobs(jobs);
    return job;
}

std::optional<Json> get_job(const std::string& job_id) {
    for (const auto& job : load_jobs()) {
        const Json* id = json_get(job, "id");
        if (id && id->type == Json::Str && id->str == job_id)
            return apply_skill_fields(job);
    }
    return std::nullopt;
}

std::vector<Json> list_jobs(bool include_disabled) {
    std::vector<Json> out;
    for (const auto& j : load_jobs()) {
        Json a = apply_skill_fields(j);
        if (!include_disabled) {
            const Json* en = json_get(a, "enabled");
            bool enabled = !(en && en->type == Json::Bool && !en->b);
            if (!enabled) continue;
        }
        out.push_back(a);
    }
    return out;
}

std::optional<Json> update_job(const std::string& job_id, const Json& updates) {
    std::vector<Json> jobs = load_jobs();
    for (size_t i = 0; i < jobs.size(); ++i) {
        const Json* id = json_get(jobs[i], "id");
        if (!(id && id->type == Json::Str && id->str == job_id)) continue;

        Json updated = apply_skill_fields(merge_json(jobs[i], updates));
        bool schedule_changed = false;
        if (json_get(updates, "schedule")) schedule_changed = true;

        if (json_get(updates, "skills") || json_get(updates, "skill")) {
            const Json* sk = json_get(updated, "skill");
            const Json* sks = json_get(updated, "skills");
            std::vector<std::string> norm = normalize_skill_list(sk, sks);
            Json arr = json_arr();
            for (const auto& s : norm) arr_push(arr, json_str(s));
            obj_set(updated, "skills", arr);
            if (!norm.empty()) obj_set(updated, "skill", json_str(norm[0]));
            else obj_set(updated, "skill", Json::make_null());
        }

        if (schedule_changed) {
            const Json* upd_sched = json_get(updated, "schedule");
            const Json* upd_disp = json_get(updates, "schedule_display");
            const Json* sched_disp = upd_sched ? json_get(*upd_sched, "display") : nullptr;
            const Json* cur_disp = json_get(updated, "schedule_display");
            Json disp;
            if (upd_disp) disp = *upd_disp;
            else if (sched_disp) disp = *sched_disp;
            else if (cur_disp) disp = *cur_disp;
            else disp = Json::make_null();
            obj_set(updated, "schedule_display", disp);

            const Json* st = json_get(updated, "state");
            bool paused = st && st->type == Json::Str && st->str == "paused";
            if (!paused) {
                auto nr = next_run(*upd_sched, std::nullopt);
                obj_set(updated, "next_run_at", nr ? opt_str_to_json(nr) : Json::make_null());
            }
        }

        const Json* en2 = json_get(updated, "enabled");
        bool enabled2 = !(en2 && en2->type == Json::Bool && !en2->b);
        const Json* st2 = json_get(updated, "state");
        bool paused2 = st2 && st2->type == Json::Str && st2->str == "paused";
        const Json* nr2 = json_get(updated, "next_run_at");
        bool no_next = !nr2 || nr2->type == Json::Null;
        if (enabled2 && !paused2 && no_next) {
            const Json* sched = json_get(updated, "schedule");
            if (sched) {
                auto nr = next_run(*sched, std::nullopt);
                obj_set(updated, "next_run_at", nr ? opt_str_to_json(nr) : Json::make_null());
            }
        }

        jobs[i] = updated;
        save_jobs(jobs);
        return apply_skill_fields(jobs[i]);
    }
    return std::nullopt;
}

std::optional<Json> pause_job(const std::string& job_id,
                              const std::optional<std::string>& reason) {
    Json upd = json_obj();
    obj_set(upd, "enabled", json_bool(false));
    obj_set(upd, "state", json_str("paused"));
    obj_set(upd, "paused_at", json_str(now_iso()));
    obj_set(upd, "paused_reason", opt_str_to_json(reason));
    return update_job(job_id, upd);
}

std::optional<Json> resume_job(const std::string& job_id) {
    auto job = get_job(job_id);
    if (!job) return std::nullopt;
    const Json* sched = json_get(*job, "schedule");
    auto nr = sched ? next_run(*sched, std::nullopt) : std::nullopt;
    Json upd = json_obj();
    obj_set(upd, "enabled", json_bool(true));
    obj_set(upd, "state", json_str("scheduled"));
    obj_set(upd, "paused_at", Json::make_null());
    obj_set(upd, "paused_reason", Json::make_null());
    obj_set(upd, "next_run_at", nr ? opt_str_to_json(nr) : Json::make_null());
    return update_job(job_id, upd);
}

std::optional<Json> trigger_job(const std::string& job_id) {
    auto job = get_job(job_id);
    if (!job) return std::nullopt;
    Json upd = json_obj();
    obj_set(upd, "enabled", json_bool(true));
    obj_set(upd, "state", json_str("scheduled"));
    obj_set(upd, "paused_at", Json::make_null());
    obj_set(upd, "paused_reason", Json::make_null());
    obj_set(upd, "next_run_at", json_str(now_iso()));
    return update_job(job_id, upd);
}

bool remove_job(const std::string& job_id) {
    std::vector<Json> jobs = load_jobs();
    size_t orig = jobs.size();
    std::vector<Json> kept;
    for (const auto& j : jobs) {
        const Json* id = json_get(j, "id");
        if (id && id->type == Json::Str && id->str == job_id) continue;
        kept.push_back(j);
    }
    if (kept.size() < orig) {
        save_jobs(kept);
        return true;
    }
    return false;
}

void mark_job_run(const std::string& job_id, bool success,
                  const std::optional<std::string>& error,
                  const std::optional<std::string>& delivery_error) {
    std::vector<Json> jobs = load_jobs();
    for (size_t i = 0; i < jobs.size(); ++i) {
        Json& job = jobs[i];
        const Json* id = json_get(job, "id");
        if (!(id && id->type == Json::Str && id->str == job_id)) continue;

        std::string ts = now_iso();
        obj_set(job, "last_run_at", json_str(ts));
        obj_set(job, "last_status", json_str(success ? "ok" : "error"));
        obj_set(job, "last_error", success ? Json::make_null() : opt_str_to_json(error));
        obj_set(job, "last_delivery_error", opt_str_to_json(delivery_error));

        const Json* rep = json_get(job, "repeat");
        if (rep && rep->type == Json::Obj) {
            Json repc = json_deep_copy(*rep);
            const Json* comp = json_get(repc, "completed");
            long long completed = 0;
            if (comp && comp->type == Json::Num) completed = (long long)comp->num;
            completed += 1;
            obj_set(repc, "completed", json_num((double)completed));
            obj_set(job, "repeat", repc);

            const Json* times = json_get(repc, "times");
            bool has_times = times && times->type == Json::Num && times->num > 0;
            if (has_times && completed >= (long long)times->num) {
                jobs.erase(jobs.begin() + i);
                save_jobs(jobs);
                return;
            }
        }

        const Json* sched = json_get(job, "schedule");
        auto nr = sched ? next_run(*sched, ts) : std::nullopt;
        obj_set(job, "next_run_at", nr ? opt_str_to_json(nr) : Json::make_null());

        if (!nr.has_value()) {
            obj_set(job, "enabled", json_bool(false));
            obj_set(job, "state", json_str("completed"));
        } else {
            const Json* st = json_get(job, "state");
            bool paused = st && st->type == Json::Str && st->str == "paused";
            if (!paused) obj_set(job, "state", json_str("scheduled"));
        }

        save_jobs(jobs);
        return;
    }
}

bool advance_next_run(const std::string& job_id) {
    std::vector<Json> jobs = load_jobs();
    for (size_t i = 0; i < jobs.size(); ++i) {
        Json& job = jobs[i];
        const Json* id = json_get(job, "id");
        if (!(id && id->type == Json::Str && id->str == job_id)) continue;

        const Json* sched = json_get(job, "schedule");
        const Json* kind = sched ? json_get(*sched, "kind") : nullptr;
        bool recurring = kind && kind->type == Json::Str &&
                         (kind->str == "cron" || kind->str == "interval");
        if (!recurring) return false;

        std::string ts = now_iso();
        auto new_next = sched ? next_run(*sched, ts) : std::nullopt;
        const Json* cur = json_get(job, "next_run_at");
        std::string cur_s = cur ? json_str_of(cur) : "";
        if (new_next.has_value() && new_next.value() != cur_s) {
            obj_set(job, "next_run_at", json_str(*new_next));
            save_jobs(jobs);
            return true;
        }
        return false;
    }
    return false;
}

std::vector<Json> get_due_jobs() {
    DT n = now();
    std::vector<Json> raw = load_jobs();
    std::vector<Json> jobs;
    for (const auto& j : raw) jobs.push_back(apply_skill_fields(j));

    std::vector<Json> due;
    bool needs_save = false;

    for (auto& job : jobs) {
        const Json* en = json_get(job, "enabled");
        bool enabled = !(en && en->type == Json::Bool && !en->b);
        if (!enabled) continue;

        const Json* nrj = json_get(job, "next_run_at");
        std::string next_run_s = nrj ? json_str_of(nrj) : "";
        if (next_run_s.empty()) {
            const Json* sched = json_get(job, "schedule");
            const Json* lrj = json_get(job, "last_run_at");
            std::optional<std::string> last =
                (lrj && lrj->type == Json::Str) ? std::optional<std::string>(lrj->str)
                                                : std::nullopt;
            auto recovered = sched ? recoverable_oneshot_run_at(*sched, n, last) : std::nullopt;
            if (!recovered) continue;
            obj_set(job, "next_run_at", json_str(*recovered));
            next_run_s = *recovered;
            for (auto& rj : raw) {
                const Json* rid = json_get(rj, "id");
                const Json* jid = json_get(job, "id");
                if (rid && jid && rid->type == Json::Str && jid->type == Json::Str &&
                    rid->str == jid->str) {
                    obj_set(rj, "next_run_at", json_str(*recovered));
                    needs_save = true;
                    break;
                }
            }
        }

        DT next_dt = parse_iso(next_run_s);
        if (next_dt.minute <= n.minute) {
            const Json* sched = json_get(job, "schedule");
            const Json* kind = sched ? json_get(*sched, "kind") : nullptr;
            bool recurring = kind && kind->type == Json::Str &&
                             (kind->str == "cron" || kind->str == "interval");
            long long grace = sched ? compute_grace_seconds(*sched, n) : 0;
            long long diff_min = n.minute - next_dt.minute;
            if (recurring && diff_min * 60 > grace) {
                auto new_next = sched ? compute_next_run(*sched, n, std::nullopt) : std::nullopt;
                if (new_next) {
                    for (auto& rj : raw) {
                        const Json* rid = json_get(rj, "id");
                        const Json* jid = json_get(job, "id");
                        if (rid && jid && rid->str == jid->str) {
                            obj_set(rj, "next_run_at", json_str(*new_next));
                            needs_save = true;
                            break;
                        }
                    }
                    continue;
                }
            }
            due.push_back(job);
        }
    }

    if (needs_save) save_jobs(raw);
    return due;
}

std::filesystem::path save_job_output(const std::string& job_id, const std::string& output) {
    ensure_dirs();
    std::filesystem::path job_out = g_output_dir / job_id;
    std::error_code ec;
    std::filesystem::create_directories(job_out, ec);
    secure_dir(job_out);

    DT n = now();
    // timestamp "%Y-%m-%d_%H-%M-%S"
    std::string iso = iso_of(n);  // YYYY-MM-DDTHH:MM:SS+00:00
    std::string ts;
    ts += iso.substr(0, 10);
    ts += '_';
    ts += iso.substr(11, 2);
    ts += '-';
    ts += iso.substr(14, 2);
    ts += '-';
    ts += iso.substr(17, 2);

    std::filesystem::path out_file = job_out / (ts + ".md");
    std::string tmpname = (job_out / ".output_XXXXXX.tmp").string();
    std::vector<char> buf(tmpname.begin(), tmpname.end());
    buf.push_back('\0');
    int fd = ::mkstemps(buf.data(), 4);  // 4 = len(".tmp")
    if (fd < 0) throw std::runtime_error("mkstemp failed");
    std::string tmp_path(buf.data());
    try {
        {
            std::ofstream out(tmp_path, std::ios::binary);
            out << output;
            out.flush();
            int ffd = ::open(tmp_path.c_str(), O_RDONLY);
            if (ffd >= 0) { ::fsync(ffd); ::close(ffd); }
        }
        std::error_code ec2;
        std::filesystem::rename(tmp_path, out_file, ec2);
        if (ec2) { ::unlink(tmp_path.c_str()); throw std::runtime_error("rename failed"); }
        secure_file(out_file);
    } catch (...) {
        ::unlink(tmp_path.c_str());
        throw;
    }
    return out_file;
}

}  // namespace cron
}  // namespace dd
