#pragma once
// DM pairing store (gateway/pairing.py). Code-based approval flow for
// authorizing new users on messaging platforms: unknown users get a one-time
// pairing code the bot owner approves via the CLI.
#include "dd/core/json.hpp"
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace dd {
namespace gateway {

// `length` characters drawn uniformly from `alphabet` out of the OS CSPRNG (getrandom, rejection sampling: no modulo
// bias). "" when the alphabet is empty or longer than 256 or the kernel cannot supply randomness; never a weaker fallback.
std::string pairing_random_code(const std::string& alphabet, int length);

class PairingStore {
public:
    PairingStore();
    ~PairingStore() = default;

    std::vector<core::Json> list_pending(const std::string& platform = "");
    bool is_approved(const std::string& platform, const std::string& user_id) const;
    std::vector<core::Json> list_approved(const std::string& platform = "") const;
    bool revoke(const std::string& platform, const std::string& user_id);
    std::optional<std::string> generate_code(const std::string& platform,
                                             const std::string& user_id,
                                             const std::string& user_name = "");
    std::optional<core::Json> approve_code(const std::string& platform,
                                           const std::string& code);
    std::vector<core::Json> list_pending(const std::string& platform = "") const;
    int clear_pending(const std::string& platform = "");

    // Internal helpers (exposed for unit testing of the security invariants).
    std::filesystem::path pairing_dir() const;
    std::filesystem::path pending_path(const std::string& platform) const;
    std::filesystem::path approved_path(const std::string& platform) const;
    std::filesystem::path rate_limit_path() const;

    core::Json load_json(const std::filesystem::path& p) const;
    void save_json(const std::filesystem::path& p, const core::Json& data);

    bool is_rate_limited(const std::string& platform, const std::string& user_id) const;
    void record_rate_limit(const std::string& platform, const std::string& user_id);
    bool is_locked_out(const std::string& platform) const;
    void record_failed_attempt(const std::string& platform);
    void cleanup_expired(const std::string& platform);
    std::vector<std::string> all_platforms(const std::string& suffix) const;
    void approve_user(const std::string& platform, const std::string& user_id,
                      const std::string& user_name = "");

private:
    mutable std::mutex mutex_;
};

}  // gateway
}  // dd
