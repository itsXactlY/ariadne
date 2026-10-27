// src/context/maze.cpp — impl of dd/context/maze.hpp.
#include "dd/context/maze.hpp"

#include "dd/context/fsutil.hpp"
#include "dd/core/json.hpp"
#include "dd/net/http.hpp"
#include "dd/tools/mcp.hpp"

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

long num(const Json& o, const char* k) {
    const Json* v = core::json_get(o, k);
    return v && v->type == Json::Num ? static_cast<long>(v->num) : 0;
}

std::string str(const Json& o, const char* k) {
    const Json* v = core::json_get(o, k);
    return v && v->type == Json::Str ? v->str : "";
}

// [{id,label,content?,similarity|activation|salience}] or {"results": [...]}
std::vector<Hit> hits_of(const std::string& text) {
    std::vector<Hit> out;
    try {
        const Json j = core::json_parse(text);
        const Json* arr = j.type == Json::Arr ? &j : core::json_get(j, "results");
        if (!arr || arr->type != Json::Arr) return out;
        for (const auto& sp : arr->arr) {
            if (!sp || sp->type != Json::Obj) continue;
            const Json& o = core::json_get(*sp, "memory") ? *core::json_get(*sp, "memory") : *sp;   // get()'s shape
            if (const Json* f = core::json_get(*sp, "found"); f && f->type == Json::Bool && !f->b) continue;
            Hit h;
            h.id = num(o, "id");
            h.label = str(o, "label");
            h.content = str(o, "content");
            for (const char* k : {"similarity", "activation", "salience", "score"})
                if (const Json* s = core::json_get(o, k); s && s->type == Json::Num) {
                    h.score = s->num;
                    break;
                }
            if (h.id || !h.label.empty()) out.push_back(std::move(h));
        }
    } catch (...) {
    }
    return out;
}

Json edges_json(const std::vector<Edge>& edges, long long created) {
    Json rows = core::json_arr();
    for (const auto& e : edges)
        core::arr_push(rows, core::json_obj({{"a_label", jsp(core::json_str(e.a))},
                                             {"b_label", jsp(core::json_str(e.b))},
                                             {"weight", jsp(core::json_num(e.weight))},
                                             {"edge_type", jsp(core::json_str(e.type))},
                                             {"created_at", jsp(core::json_num(static_cast<double>(created)))}}));
    return rows;
}

}  // namespace

// ---------------- Maze ----------------

Maze::Maze(std::string mcp_url) : url_(std::move(mcp_url)) {
    if (const size_t scheme = url_.find("://"); scheme != std::string::npos) {   // http://host:port/mcp -> /health
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

std::string Maze::call(const std::string& tool, const std::string& args_json, long timeout_ms, bool mark) {
    if (url_.empty()) return "";
    tools::MCPClient mcp;
    mcp.url = url_;
    mcp.timeout_ms = timeout_ms;
    core::Json args;
    try {
        args = core::json_parse(args_json);
    } catch (...) {
        return "";
    }
    const tools::McpResult r = mcp.call_ex(tool, args);
    const bool good = r.ok && !r.is_error;   // an empty reply that was answered is still an answer
    if (mark) up_.store(good, std::memory_order_relaxed);
    return good ? r.text : std::string();
}

std::vector<Hit> Maze::recall(const std::string& query, int limit, const std::string& scope) {
    if (trim(query).empty()) return {};
    Json args = core::json_obj({{"query", jsp(core::json_str(query))},
                                {"limit", jsp(core::json_num(limit))},
                                {"max_chars", jsp(core::json_num(600))},
                                {"rerank", jsp(core::json_bool(false))}});   // the CPU rerank costs seconds per hit
    if (!scope.empty()) core::obj_set(args, "scope", core::json_str(scope));
    return hits_of(call("mazemaker_recall", core::json_dump(args), recall_timeout_ms));
}

long Maze::remember(const std::string& label, const std::string& content) {
    const std::string text = call("mazemaker_remember",
                                  core::json_dump(core::json_obj({{"content", jsp(core::json_str(content))},
                                                                  {"label", jsp(core::json_str(label))}})),
                                  60000);
    try {
        const Json j = core::json_parse(text);
        if (const long id = num(j, "id"); id > 0) return id;
    } catch (...) {
    }
    if (!text.empty()) up_.store(false, std::memory_order_relaxed);   // answered, but stored nothing
    return -1;
}

std::vector<Hit> Maze::think(long id, int depth) {
    return hits_of(call("mazemaker_think", "{\"memory_id\":" + std::to_string(id) + ",\"depth\":" + std::to_string(depth) + "}", 15000, false));
}

std::vector<Hit> Maze::get(const std::vector<long>& ids) {
    std::vector<Hit> out;
    for (size_t i = 0; i < ids.size(); i += 25) {   // the tool takes 25 per call
        std::string list;
        for (size_t k = i; k < std::min(ids.size(), i + 25); ++k) list += (list.empty() ? "" : ",") + std::to_string(ids[k]);
        auto part = hits_of(call("mazemaker_get", "{\"memory_ids\":[" + list + "]}", 15000, false));
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

std::vector<Hit> Maze::facts(long source_id, int limit) {
    return hits_of(call("mazemaker_afe_facts", "{\"source_id\":" + std::to_string(source_id) + ",\"limit\":" + std::to_string(limit) + "}", 10000, false));
}

std::string Maze::intent(const std::string& query) {
    const std::string text = call("mazemaker_classify_intent", core::json_dump(core::json_obj({{"query", jsp(core::json_str(query))}})), 5000, false);
    try {
        const Json j = core::json_parse(text);
        if (j.type == Json::Str) return j.str;
        return str(j, "intent");
    } catch (...) {
        return "";
    }
}

std::vector<Hit> Maze::labels(const std::string& prefix, int offset, int limit) {
    return hits_of(call("mazemaker_list_by_label_prefix",
                        core::json_dump(core::json_obj({{"prefix", jsp(core::json_str(prefix))},
                                                        {"offset", jsp(core::json_num(offset))},
                                                        {"limit", jsp(core::json_num(limit))}})),
                        15000, false));
}

int Maze::connect(const std::vector<Edge>& edges) {
    if (edges.empty()) return 0;
    const std::string text = call("mazemaker_connections_import",
                                  core::json_dump(core::json_obj({{"rows", jsp(edges_json(edges, now_s()))}})), 15000, false);
    try {
        return static_cast<int>(num(core::json_parse(text), "imported"));
    } catch (...) {
        return 0;
    }
}

std::string context_block(const std::string& body) {
    if (trim(body).empty()) return "";
    return "<memory-context>\n"
           "[System note: The following is recalled memory context, NOT new user input. It is background "
           "and may be outdated: the files and tools show the current state.]\n\n" +
           trim(body) + "\n</memory-context>";
}

// ---------------- Soaker ----------------

struct Soaker::State {
    std::shared_ptr<Memory> mem;
    std::string spool;
    // The two hand-offs of the soak, both rings: slots allocated once, reused forever, no node per
    // job. Each side owns a payload by CASing its word, so neither thread waits on the other.
    //   q   written by the agent thread alone (the single-writer path), claimed by the soak worker
    //   out written by the soak worker alone, claimed by the engine thread
    // A refused q publish is not a loss: the spool is the durable overflow, counted in spooled. A
    // refused out publish is a real loss of the ledger view only — the row is already in the graph,
    // and unledgered names how many the engine's ledger never saw.
    core::SlotRing<256, Job> q;
    core::SlotRing<256, LedgerEntry> out;
    std::atomic<uint32_t> bell{0};
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> queued{0}, written{0}, spooled{0}, skipped{0}, edges{0};
    std::atomic<uint64_t> unledgered{0};   // out-ring refusals: the row is in the graph, the ledger view missed it

    std::string lock_path() const { return spool + ".lock"; }

    void spool_job(const Job& j) {
        if (spool.empty()) return;
        Json e = core::json_arr();
        for (const auto& ed : j.edges)
            core::arr_push(e, core::json_obj({{"a", jsp(core::json_str(ed.a))}, {"b", jsp(core::json_str(ed.b))},
                                              {"w", jsp(core::json_num(ed.weight))}, {"t", jsp(core::json_str(ed.type))}}));
        const std::string line = core::json_dump(core::json_obj({{"label", jsp(core::json_str(j.label))},
                                                                 {"content", jsp(core::json_str(j.content))},
                                                                 {"edges", jsp(std::move(e))}})) + "\n";
        fsx::mkdirs(std::filesystem::path(spool).parent_path());   // the work dir appears only when something has to land on disk
        const fsx::FileLock lock(lock_path());
        const int fd = fsx::open_private(spool, O_APPEND);
        if (fd < 0) return;
        const ssize_t w = ::write(fd, line.data(), line.size());   // one append: whole line or nothing torn
        ::close(fd);
        if (w == static_cast<ssize_t>(line.size())) spooled.fetch_add(1, std::memory_order_relaxed);
    }

    // the spool as lines (under the lock); false: none
    bool read_lines(std::vector<std::string>& lines) const {
        std::ifstream in(spool);
        if (!in) return false;
        lines.clear();
        for (std::string l; std::getline(in, l);) lines.push_back(std::move(l));
        return !lines.empty();
    }

    // The pod answered again: one spooled write per live write, oldest first. The lock is held to read the head and
    // again to drop it, never across the network call; a second dd on this home appends in between.
    void replay_one() {
        if (spool.empty()) return;
        std::string first;
        {
            const fsx::FileLock lock(lock_path());
            std::vector<std::string> lines;
            if (!read_lines(lines)) return;
            first = lines.front();
        }
        try {
            const Json j = core::json_parse(first);
            const std::string l = str(j, "label"), c = str(j, "content");
            if (!l.empty() && mem->remember(l, c) < 0) return;
            std::vector<Edge> edges;
            if (const Json* e = core::json_get(j, "edges"); e && e->type == Json::Arr)
                for (const auto& sp : e->arr)
                    if (sp) {
                        const Json* w = core::json_get(*sp, "w");
                        edges.push_back(Edge{str(*sp, "a"), str(*sp, "b"), w && w->type == Json::Num ? w->num : 0.5, str(*sp, "t")});
                    }
            edges_n(mem->connect(edges));
        } catch (...) {   // a torn line is dropped, not retried forever
        }
        const fsx::FileLock lock(lock_path());
        std::vector<std::string> lines;
        if (!read_lines(lines) || lines.front() != first) return;   // someone else replayed it already
        lines.erase(lines.begin());
        if (lines.empty()) {
            std::remove(spool.c_str());
            return;
        }
        const std::string tmp = spool + ".tmp";
        ::unlink(tmp.c_str());
        const int fd = fsx::open_private(tmp, O_EXCL);   // 0600: the rename must not widen the spool's mode
        if (fd < 0) return;
        std::string body;
        for (const auto& l : lines) body += l + "\n";
        const bool ok = fsx::write_all(fd, body.data(), body.size());
        ::close(fd);
        if (ok) std::rename(tmp.c_str(), spool.c_str());
        else ::unlink(tmp.c_str());
    }

    void edges_n(int n) {
        if (n > 0) edges.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
    }

    void run() {
        for (;;) {
            const uint32_t seen = bell.load(std::memory_order_acquire);
            Job j;
            if (q.claim_one(j)) {   // oldest published job first; the CAS is the ownership
                if (stop.load(std::memory_order_relaxed)) {   // quitting: no network, the spool keeps it
                    spool_job(j);
                    continue;
                }
                if (const long id = mem->remember(j.label, j.content); id > 0) {
                    written.fetch_add(1, std::memory_order_relaxed);
                    if (!out.publish(LedgerEntry{j.kind, j.label, j.session, id, j.epoch}))
                        unledgered.fetch_add(1, std::memory_order_relaxed);
                    edges_n(mem->connect(j.edges));
                    replay_one();
                } else {
                    spool_job(j);
                }
                continue;
            }
            if (stop.load(std::memory_order_acquire)) return;
            bell.wait(seen, std::memory_order_acquire);
        }
    }
};

Soaker::Soaker(std::shared_ptr<Memory> memory, std::string spool_path) : st_(std::make_shared<State>()) {
    st_->mem = std::move(memory);
    st_->spool = std::move(spool_path);
    th_ = std::thread([st = st_] { st->run(); });
}

Soaker::~Soaker() {
    st_->stop.store(true, std::memory_order_release);
    st_->bell.fetch_add(1, std::memory_order_release);
    st_->bell.notify_all();
    // The join is the sync: the worker sees stop, queued jobs drain to the spool, the
    // worker exits; only a network write in flight bounds this wait. Never throw from here.
    try {
        if (th_.joinable()) th_.join();
    } catch (...) {   // teardown path
    }
}

void Soaker::push(Job j) {
    st_->queued.fetch_add(1, std::memory_order_relaxed);
    // The ring is full, so nothing can be handed to the worker. The job is not dropped: the spool
    // takes it, which is the durable overflow the replay already drains, and spooled counts it —
    // a soak that is behind shows up as a number in the status line rather than a lost turn.
    if (!st_->q.publish(std::move(j))) {
        st_->spool_job(j);
        return;
    }
    // empty -> non-empty: wake the parked worker. The counter is the futex word, so a wake lands
    // exactly on the transition and never on a job the worker already took.
    st_->bell.fetch_add(1, std::memory_order_release);
    st_->bell.notify_one();
}

bool Soaker::take(LedgerEntry& e) { return st_->out.claim_one(e); }

Soaker::Stats Soaker::stats() const {
    return Stats{st_->queued.load(), st_->written.load(), st_->spooled.load(), st_->skipped.load(), st_->edges.load(),
                 st_->unledgered.load()};
}

// Labels stay unique per session and kind (edges resolve by label): two
// writes in one second take the next second.
long long Soaker::next_ts(long long& last) {
    last = std::max(now_s(), last + 1);
    return last;
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

void Soaker::turn(const std::string& session, const std::string& user, const std::string& assistant, int epoch) {
    if (non_answer(scrub_think(assistant))) {
        st_->skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const long long t = next_ts(last_ts_turn_);
    Job j{"turn", session, label("turn", session, t), turn_content(session, user, assistant, t), epoch, {}};
    if (!prev_turn_.empty()) j.edges.push_back(Edge{prev_turn_, j.label, 0.8, "session_next"});
    for (const auto& r : reasoning_since_turn_) j.edges.push_back(Edge{r, j.label, 0.7, "reasoned_in"});
    reasoning_since_turn_.clear();
    prev_turn_ = j.label;
    push(std::move(j));
}

void Soaker::reasoning(const std::string& session, const std::string& text, int epoch) {
    if (trim(scrub_think(text)).empty()) return;
    const long long t = next_ts(last_ts_reasoning_);
    Job j{"reasoning", session, label("reasoning", session, t), reasoning_content(session, text, t), epoch, {}};
    reasoning_since_turn_.push_back(j.label);
    push(std::move(j));
}

}  // namespace context
}  // namespace dd
