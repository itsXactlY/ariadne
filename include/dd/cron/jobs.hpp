#pragma once
// dd::cron::jobs — cron job storage/CRUD (port of cron/jobs.py).
//
// Jobs live in <daedalus_home>/cron/jobs.json; output in cron/output/{id}/{ts}.md.
// Each job is a core::Json object. Atomic writes (tempfile + fsync + rename),
// owner-only perms (0700 dirs / 0600 files), and an in-process lock serialize
// the load->modify->save critical section, matching the Python module.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include "dd/core/json.hpp"

namespace dd {
namespace cron {

// ---- paths ----
std::filesystem::path cron_dir();
std::filesystem::path jobs_file();
std::filesystem::path output_dir();

void ensure_dirs();

// ---- storage ----
std::vector<core::Json> load_jobs();
void save_jobs(const std::vector<core::Json>& jobs);

// ---- CRUD ----
struct CreateJobParams {
    std::string prompt;
    std::string schedule;
    std::optional<std::string> name;
    std::optional<long long> repeat;  // nullopt == forever
    std::optional<std::string> deliver;
    std::optional<core::Json> origin;
    std::optional<std::string> skill;
    std::optional<std::vector<std::string>> skills;
    std::optional<std::string> model;
    std::optional<std::string> provider;
    std::optional<std::string> base_url;
    std::optional<std::string> script;
};

core::Json create_job(const CreateJobParams& p);
std::optional<core::Json> get_job(const std::string& job_id);
std::vector<core::Json> list_jobs(bool include_disabled = false);
std::optional<core::Json> update_job(const std::string& job_id, const core::Json& updates);
std::optional<core::Json> pause_job(const std::string& job_id,
                                    const std::optional<std::string>& reason = std::nullopt);
std::optional<core::Json> resume_job(const std::string& job_id);
std::optional<core::Json> trigger_job(const std::string& job_id);
bool remove_job(const std::string& job_id);
void mark_job_run(const std::string& job_id, bool success,
                  const std::optional<std::string>& error = std::nullopt,
                  const std::optional<std::string>& delivery_error = std::nullopt);
bool advance_next_run(const std::string& job_id);
std::vector<core::Json> get_due_jobs();
std::filesystem::path save_job_output(const std::string& job_id, const std::string& output);

}  // namespace cron
}  // namespace dd
