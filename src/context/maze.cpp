// src/context/maze.cpp — impl of dd/context/maze.hpp.
#include "dd/context/maze.hpp"

#include "dd/core/json.hpp"
#include "dd/net/http.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

namespace dd {
namespace context {

using core::Json;

namespace {

core::JsonSP jsp(Json v) { return std::make_shared<Json>(std::move(v)); }

std::string iso_utc(long long unix_s) {
    const std::time_t t = static_cast<std::time_t>(unix_s);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return b;
}

std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string utf8_cut(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return s.substr(0, n);
}

long long now_s() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

// ---------------- Maze ----------------

Maze::Maze(std::string mcp_url) : url_(std::move(mcp_url)) {
    // http://host:port/mcp -> http://host:port/health
    if (const size_t scheme = url_.find("://"); scheme != std::string::npos) {
        const size_t path = url_.find('/', scheme + 3);
        health_ = (path == std::string::npos ? url_ : url_.substr(0, path)) + "/health";
    }
}

bool Maze::probe() {
    if (health_.empty()) return false;
    const net::Response r = net::get(health_, 3000);
    bool ok = false;
    if (r.ok()) {
        try {
            const Json j = core::json_parse(r.body);
            const Json* m = core::json_get(j, "mazemaker_reachable");
            ok = !m || m->type != Json::Bool || m->b;
        } catch (...) {
        }
    }
    up_.store(ok, std::memory_order_relaxed);
    return ok;
}

std::vector<Maze::Hit> Maze::recall(const std::string& query, int limit, bool rerank, long timeout_ms) {
    std::vector<Hit> hits;
    if (url_.empty() || trim(query).empty()) return hits;
    tools::MCPClient mcp;
    mcp.url = url_;
    mcp.timeout_ms = timeout_ms;
    Json args = core::json_obj({{"query", jsp(core::json_str(query))},
                                {"limit", jsp(core::json_num(limit))},
                                {"max_chars", jsp(core::json_num(600))},
                                {"rerank", jsp(core::json_bool(rerank))}});
    std::string text;
    try {
        text = mcp.call("mazemaker_recall", args);
    } catch (...) {
        up_.store(false, std::memory_order_relaxed);
        return hits;
    }
    if (text.empty()) {
        up_.store(false, std::memory_order_relaxed);
        return hits;
    }
    up_.store(true, std::memory_order_relaxed);
    try {
        const Json j = core::json_parse(text);
        const Json* arr = j.type == Json::Arr ? &j : core::json_get(j, "results");
        if (!arr || arr->type != Json::Arr) return hits;
        for (const auto& sp : arr->arr) {
            if (!sp || sp->type != Json::Obj) continue;
            const Json* label = core::json_get(*sp, "label");
            const Json* content = core::json_get(*sp, "content");
            if (!content || content->type != Json::Str) continue;
            Hit h;
            h.label = label && label->type == Json::Str ? label->str : "";
            h.content = content->str;
            if (const Json* s = core::json_get(*sp, "similarity"); s && s->type == Json::Num) h.similarity = s->num;
            hits.push_back(std::move(h));
        }
    } catch (...) {
    }
    return hits;
}

bool Maze::remember(const std::string& label, const std::string& content, long timeout_ms) {
    if (url_.empty()) return false;
    tools::MCPClient mcp;
    mcp.url = url_;
    mcp.timeout_ms = timeout_ms;
    std::string text;
    try {
        text = mcp.call("mazemaker_remember", core::json_obj({{"content", jsp(core::json_str(content))},
                                                              {"label", jsp(core::json_str(label))}}));
    } catch (...) {
        text.clear();
    }
    const bool ok = text.find("\"stored\"") != std::string::npos || text.find("\"id\"") != std::string::npos;
    up_.store(ok, std::memory_order_relaxed);
    return ok;
}

std::string Maze::context_block(const std::vector<Hit>& hits) {
    std::string lines;
    for (const auto& h : hits) {
        std::string body = h.content;
        std::replace(body.begin(), body.end(), '\n', ' ');
        lines += "- " + (h.label.empty() ? std::string() : "[" + h.label + "] ") + body + "\n";
    }
    if (trim(lines).empty()) return "";
    return "<memory-context>\n"
           "[System note: The following is recalled memory context, NOT new user input. It is background "
           "and may be outdated: the files and tools show the current state.]\n\n" +
           lines + "</memory-context>";
}

// ---------------- Soaker ----------------

struct Soaker::State {
    std::shared_ptr<Maze> maze;
    std::string spool;
    core::MpscQueue<Job> q;
    std::atomic<uint32_t> bell{0};
    std::atomic<bool> stop{false}, busy{false};
    std::atomic<uint64_t> queued{0}, written{0}, spooled{0}, skipped{0};

    void spool_job(const Job& j) {
        if (spool.empty()) return;
        const std::string line = core::json_dump(core::json_obj({{"label", jsp(core::json_str(j.label))},
                                                                 {"content", jsp(core::json_str(j.content))}})) + "\n";
        std::error_code ec;   // the work dir appears only when something has to land on disk
        std::filesystem::create_directories(std::filesystem::path(spool).parent_path(), ec);
        const int fd = ::open(spool.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (fd < 0) return;
        const ssize_t w = ::write(fd, line.data(), line.size());   // one append: whole line or nothing torn
        ::close(fd);
        if (w == static_cast<ssize_t>(line.size())) spooled.fetch_add(1, std::memory_order_relaxed);
    }

    // The pod answered again: one spooled write per live write, oldest first.
    void replay_one() {
        if (spool.empty()) return;
        std::ifstream in(spool);
        if (!in) return;
        std::string first, line;
        std::ostringstream rest;
        if (!std::getline(in, first)) return;
        while (std::getline(in, line)) rest << line << "\n";
        in.close();
        try {
            const Json j = core::json_parse(first);
            const Json* l = core::json_get(j, "label");
            const Json* c = core::json_get(j, "content");
            if (l && c && l->type == Json::Str && c->type == Json::Str && !maze->remember(l->str, c->str, 60000)) return;
        } catch (...) {   // a torn line is dropped, not retried forever
        }
        const std::string tmp = spool + ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            out << rest.str();
        }
        if (rest.str().empty()) std::remove(spool.c_str());
        else std::rename(tmp.c_str(), spool.c_str());
        std::remove(tmp.c_str());
    }

    void run() {
        for (;;) {
            const uint32_t seen = bell.load(std::memory_order_acquire);
            Job j;
            if (q.pop(j)) {
                if (stop.load(std::memory_order_relaxed)) {   // quitting: no network, the spool keeps it
                    spool_job(j);
                    continue;
                }
                busy.store(true, std::memory_order_release);
                if (maze->remember(j.label, j.content, 60000)) {
                    written.fetch_add(1, std::memory_order_relaxed);
                    replay_one();
                } else {
                    spool_job(j);
                }
                busy.store(false, std::memory_order_release);
                continue;
            }
            if (stop.load(std::memory_order_acquire)) return;
            bell.wait(seen, std::memory_order_acquire);
        }
    }
};

Soaker::Soaker(std::shared_ptr<Maze> maze, std::string spool_path) : st_(std::make_shared<State>()) {
    st_->maze = std::move(maze);
    st_->spool = std::move(spool_path);
    std::thread([st = st_] { st->run(); }).detach();
}

Soaker::~Soaker() {
    st_->stop.store(true, std::memory_order_release);
    st_->bell.fetch_add(1, std::memory_order_release);
    st_->bell.notify_all();
    // A write in flight gets a moment; past that the worker finishes on its
    // own (it holds the state) and anything still queued goes to the spool.
    for (int i = 0; i < 30 && st_->busy.load(std::memory_order_acquire); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void Soaker::push(Job j) {
    st_->queued.fetch_add(1, std::memory_order_relaxed);
    st_->q.push(std::move(j));
    st_->bell.fetch_add(1, std::memory_order_release);
    st_->bell.notify_one();
}

Soaker::Stats Soaker::stats() const {
    return Stats{st_->queued.load(), st_->written.load(), st_->spooled.load(), st_->skipped.load()};
}

bool Soaker::non_answer(const std::string& text) {
    const std::string t = lower(trim(text));
    if (t.empty()) return true;
    for (const char* lead : {"operation interrupted", "[request interrupted", "request interrupted", "interrupted by user"})
        if (t.rfind(lead, 0) == 0) {
            const size_t n = std::string(lead).size();
            if (t.size() == n || !std::isalnum(static_cast<unsigned char>(t[n]))) return true;
        }
    return false;
}

std::string Soaker::scrub_think(const std::string& text) {
    std::string out = text;
    for (size_t open; (open = out.find("<think>")) != std::string::npos;) {
        const size_t close = out.find("</think>", open);
        if (close == std::string::npos) {   // truncated mid-thought: the rest is reasoning
            out.resize(open);
            break;
        }
        out.erase(open, close + 8 - open);
    }
    return out;
}

std::string Soaker::label(const char* kind, const std::string& session, long long unix_s) {
    char hex[24];
    std::snprintf(hex, sizeof hex, "%llx", unix_s);
    return std::string("auto:") + kind + ":" + session + ":" + hex;
}

std::string Soaker::turn_content(const std::string& session, const std::string& user,
                                 const std::string& assistant, long long unix_s) {
    std::string c = "session:" + session + " @ " + iso_utc(unix_s) + "\n\n=== USER ===\n" + user +
                    "\n\n=== ASSISTANT ===\n" + trim(scrub_think(utf8_cut(assistant, kMaxContent)));
    if (c.size() > kMaxContent) c = utf8_cut(c, kMaxContent) + "\n…[truncated]";
    return c;
}

std::string Soaker::reasoning_content(const std::string& session, const std::string& text, long long unix_s) {
    std::string body = trim(scrub_think(text));
    if (body.size() > kMaxContent) body = utf8_cut(body, kMaxContent) + "\n…[truncated]";
    return "session:" + session + " @ " + iso_utc(unix_s) + "\n\n=== REASONING ===\n" + body;
}

void Soaker::turn(const std::string& session, const std::string& user, const std::string& assistant) {
    if (non_answer(scrub_think(assistant))) {
        st_->skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const long long t = now_s();
    push(Job{label("turn", session, t), turn_content(session, user, assistant, t)});
}

void Soaker::reasoning(const std::string& session, const std::string& text) {
    if (trim(scrub_think(text)).empty()) return;
    const long long t = now_s();
    push(Job{label("reasoning", session, t), reasoning_content(session, text, t)});
}

}  // namespace context
}  // namespace dd
