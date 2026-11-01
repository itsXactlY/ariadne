// tests/limbo_charon_test.cpp — the crossing, against a dweller that speaks the protocol.
//
// Everything Charon is: a spawned process, a pump thread that turns bytes into lines, a ring
// that hands those lines to whoever holds the turn, and a stop that lets the dweller free its
// own memory before the kernel takes it. Asserted, in one line each:
//   READY is the toll: a window, and whether a STOP line is honoured;
//   INFO facts accumulate and survive a read;
//   a turn's lines arrive in the order the engine wrote them, figures and all;
//   STOP is sent once per turn, and an abandoned turn is drained to its DONE;
//   an ERR leaves the pipe usable; a death is read as a death, not as a timeout;
//   a quiet engine is quiet, not dead;
//   a dweller that never answers READY is ended, not waited on.
#include <cstdio>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "dd/limbo/charon.hpp"

using namespace dd;
using namespace dd::limbo;

namespace {
int g_fail = 0;
void chk(bool ok, const std::string& what) {
    if (!ok) { ++g_fail; std::printf("FAIL %s\n", what.c_str()); }
    else std::printf("ok   %s\n", what.c_str());
}

const char* engine_path() { return "tests/fake_engine.sh"; }

std::string tmp_log(const std::string& name) {
    const char* tmp = std::getenv("DD_TEST_TMP");
    return std::string(tmp ? tmp : "build/tmp") + "/" + name;
}

ShadeSpec spec(const std::string& log) {
    ShadeSpec s;
    s.kind = ShadeKind::Strata;
    s.name = "fake";
    s.model_id = "fake";
    s.exe = engine_path();
    s.log = log;
    s.ready_timeout_s = 10;
    return s;
}

// The whole line stream of a turn, in the order the engine wrote it.
std::vector<EngineLine> drain(Charon& c, long timeout_ms) {
    std::vector<EngineLine> lines;
    EngineLine el;
    while (c.next(el, timeout_ms)) lines.push_back(el);
    return lines;
}

std::string field(const core::Json& o, const char* key) {
    const core::Json* v = core::json_get(o, key);
    if (!v) return std::string();
    if (v->type == core::Json::Str) return v->str;
    if (v->num == static_cast<double>(static_cast<long long>(v->num))) return std::to_string(static_cast<long long>(v->num));
    return std::to_string(v->num);
}
}  // namespace

// A fact the dweller published asynchronously: reading it immediately would race the pump, so
// the read is bounded and retried. Bounded, because a fact that never arrives is a failed test.
core::Json wait_fact(Charon& c, const char* key, long ms = 2000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        core::Json f = c.facts();
        if (core::json_get(f, key)) return f;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return c.facts();
}

int main() {
    // --- the crossing opens, and READY is the toll ---
    {
        const std::string log = tmp_log("charon_load.log");
        Charon c(spec(log));
        std::string error;
        chk(c.start(error), "a dweller that answers READY is crossed");
        chk(error.empty(), "a crossing that worked says nothing");
        chk(c.max_context() == 4096, "READY's window is kept");
        chk(c.can_stop(), "READY's `stop` token is read as: a STOP line is honoured");
        chk(c.alive() && c.pid() > 0, "the dweller is a live process");

        // INFO facts accumulate, and a read does not consume them: whoever asks next gets the
        // same set. That is what /props answering twice in a row needs.
        const core::Json facts = c.facts();
        chk(field(facts, "version") == "0.1.39", "the load's facts are readable");
        chk(field(facts, "kv_mib") == "1", "an integer fact stays an integer");
        chk(c.facts().obj.size() == facts.obj.size(), "a fact survives being read");
        chk(c.facts_dropped() == 0, "no INFO snapshot was turned away");
        chk(c.refused() == 0, "no line was turned away");

        // stderr goes where the level asked, so a crash report can be read after the process is gone.
        std::ifstream in(log);
        std::ostringstream oss;
        oss << in.rdbuf();
        const std::string kept = oss.str();
        chk(kept.find("a note the log must keep") != std::string::npos, "the dweller's stderr lands in the log");

        chk(!c.dead(), "a crossed dweller is not dead");
        c.stop();
        chk(!c.alive() && c.dead(), "stop() ends the dweller");
        c.stop();   // a second stop is a no-op, not a double reap
        chk(c.exit_code() >= 0, "the exit status was taken once, by the pump");
    }

    // --- an env the level set is the dweller's env ---
    {
        ShadeSpec s = spec(tmp_log("charon_env.log"));
        s.env.emplace_back("FAKE_CTX", "2048");
        Charon c(s);
        std::string error;
        chk(c.start(error), "a dweller spawned with a set environment is crossed");
        chk(c.max_context() == 2048, "the environment reached the dweller");
        c.stop();
    }

    // --- READY with no window is a failed crossing, not a zero window ---
    {
        ShadeSpec s = spec(tmp_log("charon_noctx.log"));
        s.env.emplace_back("FAKE_CTX", "");
        Charon c(s);
        std::string error;
        chk(!c.start(error), "a dweller that says READY with no window is refused");
        chk(error.find("no window") != std::string::npos, "and the refusal says the window is missing");
        chk(!c.alive(), "the refused dweller is not left running");
    }

    // --- a dweller that never answers READY is ended, not waited on ---
    {
        ShadeSpec s = spec(tmp_log("charon_hang.log"));
        s.exe = engine_path();
        s.env.emplace_back("FAKE_QUIET", "1");   // reads its stdin, says nothing, forever
        s.ready_timeout_s = 1;
        Charon c(s);
        const auto began = std::chrono::steady_clock::now();
        std::string error;
        chk(!c.start(error), "a dweller that is still loading past the ceiling is refused");
        chk(error.find("still loading") != std::string::npos, "the refusal names the ceiling");
        const long took = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - began).count();
        chk(took >= 900 && took < 2500, "the ceiling is honoured: the wait was the ceiling, not infinite");
        chk(!c.alive(), "and the hung dweller is ended");
    }

    // --- an exe that is not there is refused, with the name in the error ---
    {
        ShadeSpec s = spec(tmp_log("charon_missing.log"));
        s.exe = "tests/no_such_dweller";
        Charon c(s);
        std::string error;
        chk(!c.start(error), "a dweller whose binary is missing is refused");
        chk(error.find("no_such_dweller") != std::string::npos, "the refusal names the binary");
        chk(!c.alive(), "nothing was spawned");
    }

    // --- a turn: the lines arrive in the order the engine wrote them ---
    {
        Charon c(spec(tmp_log("charon_turn.log")));
        std::string error;
        chk(c.start(error), "crossed");
        chk(c.send("GEN 4 0 4096 2 1 0 0 0 -1 -1 1000 1,2,3", error), "a request line goes down the pipe");

        const std::vector<EngineLine> lines = drain(c, 2000);
        std::vector<LineKind> kinds;
        for (const auto& el : lines) kinds.push_back(el.kind);
        // PP, RESUME, then each token with its logprob, then DONE. INFO lines never reach the
        // line ring: the pump folds them into the facts, so eight lines is the whole turn.
        chk(kinds.size() == 8, "the turn's eight lines all arrived");
        chk(kinds[0] == LineKind::Progress && kinds[1] == LineKind::Resume, "the prompt's figures come first");
        chk(lines[0].read == 10 && lines[0].total == 10 && lines[0].ms == 5.0 && lines[0].tok_s == 2000.0,
            "PP's four figures are read as four figures");
        chk(lines[1].n == 3, "RESUME says how many prompt tokens were not read again");
        chk(kinds[2] == LineKind::Token && lines[2].token == 1, "the first token, with its own id");
        chk(kinds[3] == LineKind::Logprob, "its logprob follows in the same flush");
        chk(kinds[4] == LineKind::Token && lines[4].token == 2, "the second token");
        chk(kinds[5] == LineKind::Logprob, "and its logprob");
        chk(kinds[6] == LineKind::Token && lines[6].token == 0, "the stop token arrives like any other token");
        chk(kinds[7] == LineKind::Done, "the turn ends on DONE");
        chk(field(lines[7].fields, "generated") == "3", "DONE's generated count");
        chk(field(lines[7].fields, "prompt_tokens") == "10", "DONE's prompt count");
        chk(field(lines[7].fields, "prompt_ms") == "5", "DONE's prefill time");
        chk(field(lines[7].fields, "decode_ms") == "12", "DONE's decode time");
        chk(field(lines[7].fields, "finish") == "stop", "DONE's finish reason");
        chk(field(lines[7].fields, "drafts_accepted") == "2" && field(lines[7].fields, "drafts_offered") == "4",
            "the draft's hit rate comes off the same line");
        chk(field(lines[7].fields, "reused") == "5", "the conversation cache's reuse comes off the same line");
        chk(lines[7].raw.find("DONE 3 10 5.0 12.0 stop 2 4 5") == 0, "the raw line survives, so a figure is traceable");
        chk(c.refused() == 0, "nothing was dropped while a consumer read");
        c.stop();
    }

    // --- STOP is sent once per turn ---
    {
        Charon c(spec(tmp_log("charon_stop.log")));
        std::string error;
        chk(c.start(error), "crossed");
        chk(c.send("GEN 4 0 4096 2 1 0 0 0 -1 -1 1000 1,2,3", error), "a turn is in flight");
        c.stop_generation();
        c.stop_generation();
        c.stop_generation();
        const std::vector<EngineLine> lines = drain(c, 2000);
        chk(!lines.empty() && lines.back().kind == LineKind::Done, "a stopped turn still runs to its DONE");
        chk(field(wait_fact(c, "stops"), "stops") == "1", "the dweller was told once, not three times");
        // A new turn resets the latch: the next STOP must be sent again.
        chk(c.send("GEN 4 0 4096 2 1 0 0 0 -1 -1 1000 1,2,3", error), "a second turn is accepted");
        c.stop_generation();
        drain(c, 2000);
        chk(field(wait_fact(c, "stops"), "stops") == "2", "and the new turn may be stopped again");
        c.stop();
    }

    // --- an abandoned turn is drained, so the next turn reads its own lines ---
    {
        Charon c(spec(tmp_log("charon_drain.log")));
        std::string error;
        chk(c.start(error), "crossed");
        chk(c.send("GEN 4 0 4096 2 1 0 0 0 -1 -1 1000 1,2,3", error), "a turn is in flight");
        EngineLine el;
        chk(c.next(el, 1000) && el.kind == LineKind::Progress, "the consumer read the first line");
        chk(c.drain_to_done(2000, error), "the abandoned turn was drained to its DONE");
        chk(error.empty(), "draining reported nothing");
        chk(c.send("GEN 4 0 4096 2 1 0 0 0 -1 -1 1000 1,2,3", error), "the next turn is sent");
        const std::vector<EngineLine> lines = drain(c, 2000);
        chk(lines.size() == 8 && lines[2].token == 1, "and it reads only its own lines");
        c.stop();
    }

    // --- ERR: the engine refused and stayed idle, so the pipe is still usable ---
    {
        Charon c(spec(tmp_log("charon_err.log")));
        std::string error;
        chk(c.start(error), "crossed");
        chk(c.send("GEN 4 0 4096 2 1 0 0 0 -1 -1 1000 REFUSE", error), "a request the engine will refuse is sent");
        EngineLine el;
        chk(c.next(el, 2000) && el.kind == LineKind::Err, "the refusal arrives as a line");
        chk(field(el.fields, "reason") == "context too long", "and it carries the engine's reason");
        chk(c.alive(), "a refusal does not kill the dweller");
        chk(!c.drain_to_done(300, error), "there is no DONE after an ERR: draining says so");
        chk(error.find("while draining") != std::string::npos, "and says what it waited for");
        chk(c.send("GEN 4 0 4096 2 1 0 0 0 -1 -1 1000 1,2,3", error), "the next request is accepted");
        const std::vector<EngineLine> lines = drain(c, 2000);
        chk(!lines.empty() && lines.back().kind == LineKind::Done, "and it runs");
        c.stop();
    }

    // --- a quiet engine is quiet, not dead ---
    {
        Charon c(spec(tmp_log("charon_slow.log")));
        std::string error;
        chk(c.start(error), "crossed");
        chk(c.send("GEN 4 0 4096 2 1 0 0 0 -1 -1 1000 SLOW", error), "a turn that takes a second is sent");
        EngineLine el;
        chk(!c.next(el, 50), "nothing arrived inside the timeout");
        chk(!c.dead() && c.alive(), "and the dweller is alive: quiet is not death");
        chk(c.next(el, 2000) && el.kind == LineKind::Done, "the turn finishes when the engine gets there");
        chk(field(el.fields, "finish") == "length", "with its own finish reason");
        c.stop();
    }

    // --- a death mid-turn is read as a death, with the exit status ---
    {
        Charon c(spec(tmp_log("charon_crash.log")));
        std::string error;
        chk(c.start(error), "crossed");
        chk(c.send("GEN 4 0 4096 2 1 0 0 0 -1 -1 1000 CRASH", error), "a turn the dweller dies inside is sent");
        EngineLine el;
        chk(!c.next(el, 2000), "the stream ends");
        chk(c.dead() && !c.alive(), "the pump saw the end");
        chk(c.exit_code() == 7, "the exit code is the dweller's, not this process's");
        chk(!c.send("GEN 4 0 4096 2 1 0 0 0 -1 -1 1000 1,2,3", error), "a dead dweller takes no request");
        chk(error.find("not crossed") != std::string::npos, "and the refusal says the pipe is closed");
        chk(!c.drain_to_done(300, error), "draining a dead dweller says so");
        chk(error.find("exit code 7") != std::string::npos, "with the code that ended it");
        c.stop();   // already gone: stop() must not reap a pid the pump reaped
        chk(!c.alive(), "stop() after a death is a no-op");
    }

    // --- a line this decoder has no place for is kept, not lost ---
    {
        const std::string log = tmp_log("charon_other.log");
        Charon c(spec(log));
        std::string error;
        chk(c.start(error), "crossed");
        chk(c.send("SOMETHING WEIRD 1 2 3", error), "an unknown command is sent");
        // The engine's answer is an INFO line: the pump folds facts into the fact ring and keeps
        // them out of the turn's line ring, so a turn never reads a fact as a token.
        chk(c.facts().obj.size() >= 2, "the unknown command's answer lands in the facts");
        chk(field(wait_fact(c, "note"), "note").find("SOMETHING") != std::string::npos, "the answer is kept, not dropped");
        EngineLine el;
        chk(!c.next(el, 200), "and a fact does not travel as a turn's line");
        chk(c.send("VRAM 256", error), "a VRAM line is sent");
        chk(c.next(el, 2000) && el.kind == LineKind::Vram, "a VRAM line is read as VRAM");
        c.stop();
    }

    // --- a line past the ring's capacity backpressures instead of dropping ---
    {
        Charon c(spec(tmp_log("charon_backpressure.log")));
        std::string error;
        chk(c.start(error), "crossed");
        // Nobody reads while the dweller speaks 5000 tokens: the ring fills, the pump parks in the
        // kernel, and the pipe backpressures. A token is never dropped for want of a reader.
        chk(c.send("GEN 5000 0 4096 2 1 0 0 0 -1 -1 1000 FLOOD", error), "a flood of tokens is sent");
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        // A refused publish is the push-back, not a drop: the pump keeps the payload and retries,
        // and the pipe backpressures the dweller. Zero refusals would mean the ring never filled,
        // which would prove nothing about backpressure.
        chk(c.refused() > 0, "the ring pushed back on a full ring instead of dropping a token");
        chk(c.alive(), "and the pump did not stall the dweller to death");
        EngineLine el;
        long tokens = 0;
        bool saw_done = false;
        while (c.next(el, 1000)) {
            if (el.kind == LineKind::Token) ++tokens;
            if (el.kind == LineKind::Done) {
                saw_done = true;
                chk(field(el.fields, "generated") == "5000", "the flood's figures survive");
            }
        }
        chk(saw_done, "the flood ended on its DONE");
        chk(tokens == 5000, "every token of the flood survived the ring");
        c.stop();
    }

    std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "passed", g_fail);
    return g_fail ? 1 : 0;
}
