// tools/debug_helpers.py -> dd::tools
#include "dd/tools/debug_helpers.hpp"

#include "dd/core/home.hpp"
#include "dd/core/json.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <random>
#include <fstream>
#include <cctype>
#include <sstream>
#include <string>
#include <iomanip>


namespace dd {
namespace tools {

using namespace core;

namespace {

std::string to_lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// datetime.datetime.now().isoformat() -> "YYYY-MM-DDTHH:MM:SS.ffffff" (local).
std::string iso_now() {
    auto now = std::chrono::system_clock::now();
    std::time_t tt =
        std::chrono::time_point_cast<std::chrono::seconds>(now)
            .time_since_epoch().count();
    long long micro =
        std::chrono::time_point_cast<std::chrono::microseconds>(now)
            .time_since_epoch().count() % 1000000;
    std::tm tm{};
    localtime_r(&tt, &tm);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min, tm.tm_sec);
    std::ostringstream os;
    os << buf << "." << std::setfill('0') << std::setw(6) << micro;
    return os.str();
}

// uuid.uuid4() -> canonical 8-4-4-4-12 hex with version/variant nibbles.
std::string uuid4() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<int> hex(0, 15);
    static const char* H = "0123456789abcdef";
    std::string nib;
    for (int i = 0; i < 32; ++i) {
        if (i == 12) { nib += '4'; continue; }
        if (i == 16) { int x = (hex(rng) & 0x3) | 0x8; nib += H[x]; continue; }
        nib += H[hex(rng)];
    }
    std::string out;
    for (int i = 0; i < 32; ++i) {
        if (i == 8 || i == 12 || i == 16 || i == 20) out += '-';
        out += nib[i];
    }
    return out;
}

}  // anon

DebugSession::DebugSession(const std::string& tool_name, const std::string& env_var)
    : tool_name_(tool_name), env_var_(env_var) {
    const char* raw = std::getenv(env_var.c_str());
    std::string val = raw ? to_lower(raw) : "false";
    enabled_ = (val == "true");
    log_dir_ = core::get_ariadne_home() / "logs";
    if (enabled_) {
        session_id_ = uuid4();
        start_time_ = iso_now();
        std::error_code ec;
        std::filesystem::create_directories(log_dir_, ec);
    }
}

bool DebugSession::active() const { return enabled_; }

void DebugSession::log_call(const std::string& call_name, const core::Json& call_data) {
    if (!enabled_) return;
    Json entry = json_obj();
    obj_set(entry, "timestamp", json_str(iso_now()));
    obj_set(entry, "tool_name", json_str(call_name));
    // **call_data spread
    for (const auto& kv : call_data.obj) {
        obj_set(entry, kv.first, *kv.second);
    }
    calls_.push_back(entry);
}

void DebugSession::save() {
    if (!enabled_) return;
    try {
        std::string filename = tool_name_ + "_debug_" + session_id_ + ".json";
        std::filesystem::path filepath = log_dir_ / filename;
        Json payload = json_obj();
        obj_set(payload, "session_id", json_str(session_id_));
        obj_set(payload, "start_time", json_str(start_time_));
        obj_set(payload, "end_time", json_str(iso_now()));
        obj_set(payload, "debug_enabled", json_bool(true));
        obj_set(payload, "total_calls", json_num((double)calls_.size()));
        Json arr = json_arr();
        for (const auto& c : calls_) arr_push(arr, c);
        obj_set(payload, "tool_calls", arr);
        std::ofstream ofs(filepath, std::ios::binary);
        ofs << json_dump(payload);
    } catch (...) {
        // logger.error in Python — swallow, never throw.
    }
}

core::Json DebugSession::get_session_info() const {
    Json info = json_obj();
    if (!enabled_) {
        obj_set(info, "enabled", json_bool(false));
        obj_set(info, "session_id", Json::make_null());
        obj_set(info, "log_path", Json::make_null());
        obj_set(info, "total_calls", json_num(0.0));
        return info;
    }
    std::string filename = tool_name_ + "_debug_" + session_id_ + ".json";
    obj_set(info, "enabled", json_bool(true));
    obj_set(info, "session_id", json_str(session_id_));
    obj_set(info, "log_path", json_str((log_dir_ / filename).string()));
    obj_set(info, "total_calls", json_num((double)calls_.size()));
    return info;
}

}  // tools
}  // dd
