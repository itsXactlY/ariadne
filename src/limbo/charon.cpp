// src/limbo/charon.cpp — impl of dd/limbo/charon.hpp.
#include "dd/limbo/charon.hpp"

#include "dd/core/park.hpp"

#include "dd/limbo/spawn.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <chrono>
#include <climits>
#include <cstring>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

namespace dd {
namespace limbo {

namespace {

std::string last_field(std::string_view line) {
    const size_t sp = line.find(' ');
    return sp == std::string_view::npos ? std::string() : std::string(line.substr(sp + 1));
}

bool is_int(std::string_view s) {
    if (s.empty()) return false;
    size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (i == s.size()) return false;
    for (; i < s.size(); ++i)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

long to_long(std::string_view s) {
    const long long v = std::strtoll(std::string(s).c_str(), nullptr, 10);
    return static_cast<long>(v);
}

EngineLine decode(std::string line) {
    EngineLine out;
    out.raw = std::move(line);
    const std::string_view l = out.raw;
    if (l == "READY" || l.starts_with("READY ")) {
        out.kind = LineKind::Ready;
        std::vector<std::string> parts;
        size_t i = l.find(' ');
        while (i != std::string::npos) {
            size_t j = l.find(' ', i + 1);
            parts.push_back(std::string(l.substr(i + 1, j == std::string::npos ? std::string_view::npos : j - i - 1)));
            i = j;
        }
        if (!parts.empty() && is_int(parts[0])) out.n = to_long(parts[0]);
        out.fields = core::json_obj();
        for (size_t k = 1; k < parts.size(); ++k) core::obj_set(out.fields, parts[k], core::json_bool(true));
    } else if (l.starts_with("INFO ")) {
        out.kind = LineKind::Info;
        out.fields = info_fields(l.substr(5));
    } else if (l.starts_with("T ")) {
        out.kind = LineKind::Token;
        out.token = is_int(l.substr(2)) ? to_long(l.substr(2)) : -1;
    } else if (l.starts_with("LP ")) {
        out.kind = LineKind::Logprob;
    } else if (l.starts_with("PP ")) {
        out.kind = LineKind::Progress;
        std::vector<std::string> parts;
        size_t i = 3;
        while (i < l.size()) {
            size_t j = l.find(' ', i);
            parts.push_back(std::string(l.substr(i, j == std::string_view::npos ? std::string_view::npos : j - i)));
            if (j == std::string_view::npos) break;
            i = j + 1;
        }
        if (parts.size() >= 2 && is_int(parts[0]) && is_int(parts[1])) {
            out.read = to_long(parts[0]);
            out.total = to_long(parts[1]);
        }
        if (parts.size() >= 3) out.ms = std::strtod(parts[2].c_str(), nullptr);
        if (parts.size() >= 4) out.tok_s = std::strtod(parts[3].c_str(), nullptr);
    } else if (l.starts_with("RESUME ")) {
        out.kind = LineKind::Resume;
        out.n = is_int(l.substr(7)) ? to_long(l.substr(7)) : 0;
    } else if (l.starts_with("DONE")) {
        out.kind = LineKind::Done;
        out.fields = done_fields(l.substr(4));
    } else if (l.starts_with("ERR")) {
        out.kind = LineKind::Err;
        out.fields = core::json_obj();
        core::obj_set(out.fields, "reason", core::json_str(last_field(l)));
    } else if (l.starts_with("VRAM")) {
        out.kind = LineKind::Vram;
        out.fields = info_fields(l.substr(4));
    } else if (l.starts_with("YIELDED ") || l.starts_with("BYIELD ")) {
        out.kind = LineKind::Yielded;
    } else if (l.starts_with("BT ") || l.starts_with("BDONE ") || l.starts_with("BADM ") || l.starts_with("BCOK ")) {
        out.kind = LineKind::Batch;
    }
    return out;
}

// Write every byte of `line`, retrying what a blocking pipe left unwritten. A dweller that
// died mid-write makes the next write EPIPE, which the caller reads as a death.
bool write_line(int fd, std::string_view line, std::string& error) {
    size_t done = 0;
    while (done < line.size()) {
        const ssize_t n = ::write(fd, line.data() + done, line.size() - done);
        if (n > 0) {
            done += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        error = n == 0 ? "the pipe closed" : std::string("pipe: ") + std::strerror(errno);
        return false;
    }
    return true;
}


}  // namespace

// shade_kind_name lives in src/limbo/shade.cpp: it is the contract's word, and the level's front
// and the judge both read it. Defining it here would make the pipe own a name nobody in it uses.

core::Json done_fields(std::string_view line) {
    core::Json o = core::json_obj();
    std::vector<std::string> f;
    // The verb and its fields are separated by a space the caller has already cut at, so the
    // remainder starts with it: a field list that begins empty shifts every positional figure
    // one place left, which is a wrong figure, not a missing one.
    size_t i = line.find_first_not_of(" \t");
    if (i == std::string_view::npos) return o;
    while (i < line.size()) {
        size_t j = line.find(' ', i);
        f.push_back(std::string(line.substr(i, j == std::string_view::npos ? std::string_view::npos : j - i)));
        if (j == std::string_view::npos) break;
        i = j + 1;
    }
    auto num = [&](size_t k) { return k < f.size() ? core::json_num(std::strtod(f[k].c_str(), nullptr)) : core::json_null(); };
    auto integer = [&](size_t k) { return k < f.size() ? core::json_num(static_cast<double>(to_long(f[k]))) : core::json_null(); };
    if (f.size() >= 1) core::obj_set(o, "generated", integer(0));
    if (f.size() >= 2) core::obj_set(o, "prompt_tokens", integer(1));
    if (f.size() >= 3) core::obj_set(o, "prompt_ms", num(2));
    if (f.size() >= 4) core::obj_set(o, "decode_ms", num(3));
    if (f.size() >= 5) core::obj_set(o, "finish", core::json_str(f[4]));
    if (f.size() >= 6) core::obj_set(o, "drafts_accepted", integer(5));   // conversation cache (0.1.3+)
    if (f.size() >= 7) core::obj_set(o, "drafts_offered", integer(6));
    if (f.size() >= 8) core::obj_set(o, "reused", integer(7));
    if (f.size() >= 10) {                                                  // decode hit rate
        core::obj_set(o, "hits", integer(8));
        core::obj_set(o, "lookups", integer(9));
    }
    if (f.size() >= 13) {                                                  // expert tiers (0.1.31+)
        core::obj_set(o, "ram_blobs", integer(10));
        core::obj_set(o, "file_blobs", integer(11));
        core::obj_set(o, "file_mb", num(12));
    }
    if (f.size() >= 14) core::obj_set(o, "prompt_read", integer(13));      // #471 (0.1.36+)
    if (f.size() >= 15) core::obj_set(o, "offloaded", integer(14));        // #588 (0.1.39+)
    for (size_t k = 15; k < f.size(); ++k) {                               // key=value after the positional ones
        const std::string& tok = f[k];
        const size_t eq = tok.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = tok.substr(0, eq), val = tok.substr(eq + 1);
        if (key == "chunk" || key == "experts_streamed") core::obj_set(o, key, core::json_num(static_cast<double>(to_long(val))));
        else core::obj_set(o, key, core::json_str(val));
    }
    return o;
}

core::Json info_fields(std::string_view line) {
    core::Json o = core::json_obj();
    size_t i = line.find_first_not_of(" \t");
    if (i == std::string_view::npos) return o;
    while (i < line.size()) {
        size_t j = line.find(' ', i);
        std::string tok = std::string(line.substr(i, j == std::string_view::npos ? std::string_view::npos : j - i));
        // The last token is a fact like every other one: breaking before storing it would drop
        // the final key of every INFO line, and a single-key line would say nothing at all.
        const size_t eq = tok.find('=');
        if (eq != std::string_view::npos) {
            const std::string key = tok.substr(0, eq), val = std::string(tok.substr(eq + 1));
            if (is_int(val)) core::obj_set(o, key, core::json_num(static_cast<double>(to_long(val))));
            else core::obj_set(o, key, core::json_str(val));
        }
        if (j == std::string_view::npos) break;
        i = j + 1;
    }
    return o;
}

Charon::Charon(ShadeSpec spec) : spec_(std::move(spec)) {}

Charon::~Charon() { stop(); }

bool Charon::start(std::string& error) {
    if (alive()) {
        error = "the dweller is already crossed";
        return false;
    }
    reap_leftover(spec_.exe, ::getpid());

    int in[2], out[2];
    if (::pipe2(in, O_CLOEXEC) != 0) {
        error = std::string("pipe: ") + std::strerror(errno);
        return false;
    }
    if (::pipe2(out, O_CLOEXEC) != 0) {
        error = std::string("pipe: ") + std::strerror(errno);
        ::close(in[0]);
        ::close(in[1]);
        return false;
    }
    std::string sink_note;
    const int log_fd = open_log_sink(spec_.log, sink_note);
    if (log_fd < 0) {
        error = "the dweller's stderr has nowhere to go: " + std::string(std::strerror(errno));
        ::close(in[0]); ::close(in[1]); ::close(out[0]); ::close(out[1]);
        return false;
    }
    // A log that could not be opened is said on stderr and never turns into a refusal: the
    // dweller still has somewhere for stderr to go. It rides `error` only when the spawn fails.
    // The child ends stay open until exec: dup2 takes the descriptors the child becomes, so
    // closing them here would spawn a dweller whose stdin and stdout are gone.
    SpawnStdio stdio;
    stdio.in_read = in[0];
    stdio.out_write = out[1];
    stdio.log_fd = log_fd;
    std::vector<std::string> argv;
    argv.push_back(spec_.exe);
    argv.push_back("--serve");
    for (const auto& a : spec_.args) argv.push_back(a);
    const pid_t pid = spawn_process(spec_.exe, argv, spec_.cwd, spec_.env, stdio, error);
    ::close(in[0]);
    ::close(out[1]);
    if (pid < 0) {
        ::close(in[1]); ::close(out[0]); ::close(log_fd);
        in_fd_ = out_fd_ = log_fd_ = -1;
        if (!sink_note.empty()) error += " (" + sink_note + ")";
        return false;
    }
    log_fd_ = log_fd;
    in_fd_ = in[1];
    out_fd_ = out[0];
    pid_.store(pid, std::memory_order_release);

    stop_sent_.store(false, std::memory_order_relaxed);
    dead_.store(false, std::memory_order_release);
    pump_ = std::thread([this] { pump(); });

    // The crossing needs a toll before it counts as open: READY says the window and whether a
    // STOP line is honoured. A load past the ceiling is ended rather than held — a hung
    // dweller holding a turn is worse than a dweller that is not running.
    const long ceiling_ms = spec_.ready_timeout_s > 0 ? static_cast<long>(spec_.ready_timeout_s) * 1000 : 0;
    const auto began = std::chrono::steady_clock::now();
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (ceiling_ms > 0 && std::chrono::duration_cast<std::chrono::milliseconds>(now - began).count() >= ceiling_ms) {
            error = spec_.exe + " is still loading after " + std::to_string(spec_.ready_timeout_s) + " s";
            // A dweller that never answered READY holds no arena yet, so the grace a loaded
            // engine is given would only make whoever waits for this load wait longer.
            stop(250);
            return false;
        }
        EngineLine el;
        if (next(el, 1000)) {
            if (el.kind == LineKind::Ready) {
                if (ctx_.load(std::memory_order_acquire) <= 0) {
                    error = spec_.exe + " said READY with no window: " + el.raw;
                    stop();
                    return false;
                }
                return true;
            }
            if (el.kind == LineKind::Closed) {
                error = spec_.exe + " ended while loading (exit code " +
                        std::to_string(exit_.load(std::memory_order_acquire)) + ")";
                return false;
            }
            continue;   // INFO and unknown lines before READY: folded or logged, keep waiting
        }
        if (dead_.load(std::memory_order_acquire)) {
            error = spec_.exe + " ended while loading (exit code " +
                    std::to_string(exit_.load(std::memory_order_acquire)) + ")";
            return false;
        }
    }
}

void Charon::pump() {
    std::string buf;
    std::vector<char> chunk(8192);
    while (true) {
        const ssize_t n = ::read(out_fd_, chunk.data(), chunk.size());
        if (n > 0) {
            buf.append(chunk.data(), static_cast<size_t>(n));
            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                if (line.empty()) continue;
                if (line.back() == '\r') line.pop_back();
                EngineLine el = decode(line);
                if (el.kind == LineKind::Ready) {
                    ctx_.store(el.n > 0 ? static_cast<int>(el.n) : 0, std::memory_order_release);
                    stop_ok_.store(el.fields.type == core::Json::Obj && core::json_get(el.fields, "stop") != nullptr,
                                   std::memory_order_release);
                }
                if (el.kind == LineKind::Info) {
                    // Facts accumulate: this line's keys land in the pump's own map, and the
                    // WHOLE map goes out as a snapshot. Whoever asks gets the complete set, and
                    // a snapshot nobody read is dropped and counted rather than stalled on.
                    for (const auto& kv : el.fields.obj) {
                        auto it = std::find_if(info_.begin(), info_.end(),
                                               [&](const auto& p) { return p.first == kv.first; });
                        if (it == info_.end()) info_.emplace_back(kv.first, *kv.second);
                        else it->second = *kv.second;
                    }
                    core::Json snap = core::json_obj();
                    for (const auto& kv : info_) core::obj_set(snap, kv.first, kv.second);
                    if (!facts_.publish(std::move(snap))) facts_dropped_.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (el.kind == LineKind::Err || el.kind == LineKind::Other) {
                    // A line the protocol has no place for is kept in the log, where a crash
                    // report can read it; it still travels as a line, so nothing vanishes.
                    if (log_fd_ >= 0) {
                        const std::string note = "[stdout] " + el.raw + "\n";
                        ::write(log_fd_, note.data(), note.size());
                    }
                }
                const uint32_t seen = space_.load(std::memory_order_relaxed);
                while (!lines_.publish(std::move(el))) {
                    // The ring is full because nobody is reading. Parking in the kernel keeps
                    // the pipe backpressured instead of dropping a token, and a claim wakes it.
                    core::park(space_, seen, std::chrono::milliseconds(50));
                    if (dead_.load(std::memory_order_acquire)) break;
                }
                core::unpark(space_, INT_MAX);
                continue;
            }
            continue;
        }
        if (n == 0) break;                       // EOF: the dweller closed its stdout
        if (errno == EINTR) continue;
        break;                                   // a broken pipe: the dweller is gone
    }
    int status = 0;
    const pid_t pid = pid_.load(std::memory_order_acquire);
    if (pid > 0) {
        // The pump is the one process that reaps a dweller: stop() waits on `ended_` instead
        // of waitpid-ing the same pid, so a status is never lost to a second waiter.
        settle_exit(pid, 2000, status);
        exit_.store(WIFEXITED(status) ? WEXITSTATUS(status) : -1, std::memory_order_release);
    }
    dead_.store(true, std::memory_order_release);
    ended_.fetch_add(1, std::memory_order_release);
    EngineLine closed;
    closed.kind = LineKind::Closed;
    closed.raw = "the dweller ended";
    lines_.publish(std::move(closed));           // the waiting thread reads the end, not a timeout
    core::unpark(space_, INT_MAX);
}

bool Charon::alive() const {
    return pid_.load(std::memory_order_acquire) > 0 && !dead_.load(std::memory_order_acquire);
}

core::Json Charon::facts() const {
    const std::vector<core::Json> got = facts_.claim_all();
    if (got.empty()) return core::json_obj();
    core::Json newest = got.back();   // claim_all is oldest-first: the newest snapshot answers
    // Put it back: the claim was ownership, and the facts belong to the dweller, not to the
    // reader. publish_any because whoever reads /props is not the pump.
    facts_.publish_any(newest);
    return newest;
}

bool Charon::send(std::string_view line, std::string& error) {
    if (!alive()) {
        error = "the dweller is not crossed; this turn was not sent";
        return false;
    }
    if (line.starts_with("GEN") || line.starts_with("BGEN")) stop_sent_.store(false, std::memory_order_relaxed);
    // A string_view cannot be concatenated: the newline is part of the command, not a footer.
    if (!write_line(in_fd_, std::string(line) + "\n", error)) {
        dead_.store(true, std::memory_order_release);
        return false;
    }
    return true;
}

void Charon::stop_generation() {
    bool no = false;
    if (!stop_sent_.compare_exchange_strong(no, true, std::memory_order_acq_rel, std::memory_order_relaxed)) return;
    std::string error;
    send("STOP", error);   // a dweller that is already gone needs no STOP; its death is read elsewhere
}

bool Charon::next(EngineLine& out, long timeout_ms) {
    if (dead_.load(std::memory_order_acquire) && lines_.published() == 0) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true) {
        if (lines_.claim_one(out)) {
            space_.fetch_add(1, std::memory_order_relaxed);
            core::unpark(space_, INT_MAX);       // the pump may be parked on a full ring
            return out.kind != LineKind::Closed;
        }
        if (dead_.load(std::memory_order_acquire)) return false;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return false;
        const uint32_t seen = space_.load(std::memory_order_relaxed);
        core::park(space_, seen, std::chrono::milliseconds(20));
    }
}

bool Charon::drain_to_done(long timeout_ms, std::string& error) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        EngineLine line;
        if (!next(line, 1000)) {
            if (dead_.load(std::memory_order_acquire)) {
                error = "the dweller ended (exit code " + std::to_string(exit_.load(std::memory_order_acquire)) + ")";
                return false;
            }
            continue;
        }
        if (line.kind == LineKind::Done) return true;
        if (line.kind == LineKind::Err) {
            error = core::json_get(line.fields, "reason") ? core::json_get(line.fields, "reason")->str : line.raw;
            return false;
        }
        if (line.kind == LineKind::Closed) {
            error = "the dweller ended (exit code " + std::to_string(exit_.load(std::memory_order_acquire)) + ")";
            return false;
        }
    }
    error = "the dweller said nothing for " + std::to_string(timeout_ms / 1000) + " s while draining";
    return false;
}

void Charon::stop(long grace_ms) {
    const pid_t pid = pid_.load(std::memory_order_acquire);
    if (pid > 0 && !dead_.load(std::memory_order_acquire)) {
        // QUIT lets the dweller free its own memory: unpinning tens of GB takes seconds, and
        // killing it first leaves the arena pinned until the kernel reclaims it — which is why
        // the next spawn would refuse to load.
        std::string error;
        send("STOP", error);
        send("QUIT", error);
        // The pump reaps it and says so on `ended_`. Past the grace the dweller is forced:
        // a process that will not free its own memory is a process the next spawn cannot load.
        const uint32_t seen = ended_.load(std::memory_order_acquire);
        for (long waited = 0; waited < grace_ms && ended_.load(std::memory_order_acquire) == seen; waited += 25)
            core::park(ended_, seen, std::chrono::milliseconds(25));
        if (ended_.load(std::memory_order_acquire) == seen) end_process(pid, 5000, error);
    }
    dead_.store(true, std::memory_order_release);
    core::unpark(space_, INT_MAX);
    if (pump_.joinable()) pump_.join();   // the pump ends at EOF, so joining cannot hang on a live pipe
    close_fds();
    pid_.store(-1, std::memory_order_release);
}

void Charon::close_fds() {
    if (in_fd_ >= 0) { ::close(in_fd_); in_fd_ = -1; }
    if (out_fd_ >= 0) { ::close(out_fd_); out_fd_ = -1; }
    if (log_fd_ >= 0) { ::close(log_fd_); log_fd_ = -1; }
}

}  // namespace limbo
}  // namespace dd
