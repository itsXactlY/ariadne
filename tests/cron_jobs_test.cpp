// tests/cron_jobs_test.cpp — exercises the cron CRUD round-trip.
#include "dd/cron/jobs.hpp"
#include "dd/cron/schedule.hpp"
#include "dd/core/home.hpp"
#include "dd/core/json.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace dd;
using namespace dd::core;
using namespace dd::cron;

static std::string sget(const Json& o, const std::string& k) {
    const Json* v = json_get(o, k);
    if (v && v->type == Json::Str) return v->str;
    return "";
}
static bool bget(const Json& o, const std::string& k, bool def) {
    const Json* v = json_get(o, k);
    if (v && v->type == Json::Bool) return v->b;
    return def;
}
static long long nget(const Json& o, const std::string& k, long long def) {
    const Json* v = json_get(o, k);
    if (v && v->type == Json::Num) return (long long)v->num;
    return def;
}
static std::vector<std::string> arr_of(const Json& o, const std::string& k) {
    std::vector<std::string> out;
    const Json* v = json_get(o, k);
    if (v && v->type == Json::Arr)
        for (const auto& e : v->arr)
            if (e->type == Json::Str) out.push_back(e->str);
    return out;
}

int main() {
    // Isolated home for the test.
    std::string home = "/tmp/dd_cron_jobs_test";
    std::error_code ec;
    std::filesystem::remove_all(home, ec);
    set_daedalus_home_override(home);

    // 1) create_job: skill normalization + defaults.
    CreateJobParams p;
    p.prompt = "hello world";
    p.schedule = "every 1d";
    p.skill = "my-skill";
    Json job = create_job(p);

    assert(job.type == Json::Obj);
    std::string id = sget(job, "id");
    assert(id.size() == 12);
    assert(sget(job, "name") == "hello world");
    assert(sget(job, "deliver") == "local");
    assert(bget(job, "enabled", true));
    assert(sget(job, "state") == "scheduled");
    assert(arr_of(job, "skills") == std::vector<std::string>{"my-skill"});
    assert(sget(job, "skill") == "my-skill");
    // next_run_at present (non-null) for @daily.
    const Json* nr = json_get(job, "next_run_at");
    assert(nr && nr->type == Json::Str);

    // 2) load_jobs round-trip.
    auto jobs = load_jobs();
    assert(jobs.size() == 1);
    assert(sget(jobs[0], "id") == id);

    // 3) get_job aligns skill fields.
    auto got = get_job(id);
    assert(got.has_value());
    assert(arr_of(*got, "skills") == std::vector<std::string>{"my-skill"});

    // 4) update_job changes name.
    Json upd = json_obj();
    obj_set(upd, "name", json_str("renamed"));
    auto up = update_job(id, upd);
    assert(up.has_value());
    assert(sget(*up, "name") == "renamed");

    // 5) mark_job_run increments completed, sets status.
    mark_job_run(id, true);
    auto after = get_job(id);
    assert(after.has_value());
    assert(sget(*after, "last_status") == "ok");
    const Json* rep = json_get(*after, "repeat");
    assert(rep && rep->type == Json::Obj);
    assert(nget(*rep, "completed", 0) == 1);

    // 6) list_jobs sees the job.
    assert(list_jobs().size() == 1);

    // 7) multi-skill normalization dedups + drops empties.
    CreateJobParams p2;
    p2.prompt = "multi";
    p2.schedule = "every 1h";
    p2.skills = std::vector<std::string>{"a", "b", "a", ""};
    Json job2 = create_job(p2);
    assert(arr_of(job2, "skills") == std::vector<std::string>{"a", "b"});
    assert(sget(job2, "skill") == "a");
    assert(load_jobs().size() == 2);

    // 8) pause/resume.
    auto paused = pause_job(id, "manual");
    assert(paused.has_value());
    assert(sget(*paused, "state") == "paused");
    assert(!bget(*paused, "enabled", true));
    auto resumed = resume_job(id);
    assert(resumed.has_value());
    assert(sget(*resumed, "state") == "scheduled");
    assert(bget(*resumed, "enabled", true));

    // 9) remove both jobs.
    assert(remove_job(id));
    assert(remove_job(sget(job2, "id")));
    assert(list_jobs().empty());

    // 10) atomic save left a jobs.json file.
    assert(std::filesystem::exists(jobs_file()));

    std::cout << "all cron-jobs tests passed\n";
    return 0;
}
