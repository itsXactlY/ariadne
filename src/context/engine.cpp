// src/context/engine.cpp — impl of dd/context/engine.hpp.
#include "dd/context/engine.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace dd {
namespace context {

using core::Message;

namespace {

std::string squash(const std::string& s) {
    std::string o;
    bool sp = false;
    for (char c : s) {
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
            sp = !o.empty();
            continue;
        }
        if (sp) o.push_back(' ');
        sp = false;
        o.push_back(c);
    }
    return o;
}

std::string utf8_cut(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return s.substr(0, n);
}

std::string thousands(long v) {
    std::string d = std::to_string(v < 0 ? -v : v), o;
    for (size_t i = 0; i < d.size(); ++i) {
        if (i && (d.size() - i) % 3 == 0) o.push_back(',');
        o.push_back(d[i]);
    }
    return v < 0 ? "-" + o : o;
}

// chars/4 of what a message puts on the wire
long est_tokens(const Item& it) {
    size_t c = it.msg.content.size() + it.msg.reasoning.size() + 16;
    for (const auto& tc : it.msg.tool_calls) c += tc.name.size() + tc.arguments.size() + 24;
    return static_cast<long>(c / 4);
}

long index_of(const std::vector<ItemPtr>& v, const ItemPtr& p) {
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i] == p) return static_cast<long>(i);
    return -1;
}

}  // namespace

// ---------------- free helpers ----------------

bool trivial_prompt(const std::string& text) {
    std::string t;
    for (char c : text) t.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    const size_t b = t.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return true;
    t = t.substr(b);
    if (t[0] == '/') {   // "/effort high" is a command, "/home/x/y fix it" is a question
        const size_t sp = t.find_first_of(" \t\r\n");
        return t.find('/', 1) == std::string::npos || t.find('/', 1) > (sp == std::string::npos ? t.size() : sp);
    }
    while (!t.empty() && std::string(" \t\r\n!?.:;,\"'~()[]{}<>*&^%$#@+=`").find(t.back()) != std::string::npos) t.pop_back();
    static const char* const kTrivial[] = {
        "yes", "no", "ok", "okay", "sure", "thanks", "thank you", "y", "n", "yep", "nope", "yeah", "nah", "hi", "hey",
        "hello", "yo", "sup", "continue", "go ahead", "do it", "proceed", "got it", "cool", "nice", "great", "done",
        "next", "lgtm", "k"};
    for (const char* w : kTrivial)
        if (t == w) return true;
    return false;
}

bool parse_overflow(const std::string& what, long* prompt, long* window) {
    std::string t;
    for (char c : what) t.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    const auto has = [&](const char* s) { return t.find(s) != std::string::npos; };
    if (!has("context") || !(has("exceed") || has("too long") || has("too large") || has("maximum context length")))
        return false;
    long p = 0, w = 0;
    // "prompt (61447 tokens)", "request (65547 tokens)": the first count that ends in " tokens)"
    if (const size_t k = t.find(" tokens)"); k != std::string::npos) {
        size_t b = k;
        while (b > 0 && std::isdigit(static_cast<unsigned char>(t[b - 1]))) --b;
        if (b < k) p = std::atol(t.substr(b, k - b).c_str());
    }
    // "in the context (61337)", "exceeds the context (61337)", "context size (65536 tokens)"
    for (const std::string key : {"context (", "context size ("}) {
        const size_t k = t.find(key);
        if (k == std::string::npos) continue;
        const size_t b = k + key.size();
        size_t e = b;
        while (e < t.size() && std::isdigit(static_cast<unsigned char>(t[e]))) ++e;
        if (e > b) {
            w = std::atol(t.substr(b, e - b).c_str());
            break;
        }
    }
    if (prompt) *prompt = p;
    if (window) *window = w;
    return true;
}

std::string anchored_query(const std::vector<ItemPtr>& items, long current) {
    if (current < 0 || current >= static_cast<long>(items.size())) return "";
    const std::string q = squash(items[static_cast<size_t>(current)]->msg.content);
    if (q.size() >= 40) return q;
    for (long i = current - 1; i >= 0; --i) {
        const Item& it = *items[static_cast<size_t>(i)];
        if (it.msg.role == "user" && !it.hidden && squash(it.msg.content).size() >= 40)
            return utf8_cut(squash(it.msg.content), 300) + "\n" + q;
    }
    return q;
}

std::string turn_so_far(const std::vector<ItemPtr>& dropped) {
    std::vector<std::string> calls, notes;
    for (const auto& it : dropped) {
        if (it->msg.role != "assistant") continue;
        for (const auto& c : it->msg.tool_calls) calls.push_back(Renderer::describe_call(c));
        if (const std::string t = squash(it->msg.content); !t.empty()) notes.push_back(utf8_cut(t, 200));
    }
    if (calls.empty() && notes.empty()) return "";
    const std::string head = "[this turn so far] Before the rebase this turn already ran, in order: ";
    const std::string foot = ". Their outputs are not kept; use what the tail below shows, and run a call again only "
                             "if you need its output.";
    long budget = 2400 - static_cast<long>(head.size() + foot.size());
    std::vector<std::string> kept;
    for (size_t i = calls.size(); i-- > 0;) {   // the most recent calls matter most
        if (static_cast<long>(calls[i].size()) + 2 > budget) {
            kept.push_back("…" + std::to_string(calls.size() - kept.size()) + " earlier calls");
            break;
        }
        kept.push_back(calls[i]);
        budget -= static_cast<long>(calls[i].size()) + 2;
    }
    std::string out = "[this turn so far]";
    if (!kept.empty()) {
        out = head;
        for (size_t i = kept.size(); i-- > 0;) out += kept[i] + (i ? "; " : "");
        out += foot;
    }
    if (!notes.empty()) {
        out += "\nYour notes this turn: ";
        for (size_t i = notes.size() > 3 ? notes.size() - 3 : 0; i < notes.size(); ++i)
            out += notes[i] + (i + 1 < notes.size() ? " | " : "");
    }
    return out;
}

// ---------------- TapeEngine ----------------

TapeEngine::TapeEngine(EngineConfig cfg, std::string system_prompt, Endpoint& ep, std::shared_ptr<Memory> memory,
                       Soaker* soaker, ToolDeck& deck, agent::HttpLLM& llm, std::string spill_root,
                       know::Router* router)
    : cfg_(cfg), system_(std::move(system_prompt)), ep_(ep), mem_(std::move(memory)), soaker_(soaker), router_(router),
      recaller_(mem_), deck_(deck),
      llm_(llm), spill_root_(std::move(spill_root)), session_(std::make_unique<Session>(Session::new_id())),
      spill_(std::make_unique<Spill>(spill_root_ + "/" + session_->id())),
      render_(cfg.render, [m = mem_] { return m && m->up(); }, [this](Item& it) { soak(it); }, spill_.get()) {
    Spill::sweep(spill_root_);
    publish();
}

TapeEngine::~TapeEngine() {
    join_prep(true);
    delete ready_.exchange(nullptr);
    spill_->remove_all();
}

long TapeEngine::window() const {
    if (const long c = ep_.info().n_ctx; c > 0) return c;
    return cfg_.render.context_length > 0 ? cfg_.render.context_length : window_seen_.load(std::memory_order_relaxed);
}

// The fast layer is a budget inside the server's window, never past it: a server window cut below it (Strata's
// --max-context) would put the hard mark where the server already refuses. A sixteenth stays free for the reply.
long TapeEngine::fast_layer() const {
    long f = cfg_.fast_layer_tokens;
    if (f <= 0) return 0;
    if (const long w = window(); w > 0) f = std::min(f, w - w / 16);
    return f;
}

long TapeEngine::rebase_at() const {
    if (const long f = fast_layer(); f > 0) return static_cast<long>(static_cast<double>(f) * cfg_.hard);
    const long ctx = window();
    return ctx > 0 ? static_cast<long>(static_cast<double>(ctx) * cfg_.ceiling) : 0;
}

long TapeEngine::high_at() const {
    if (const long f = fast_layer(); f > 0) return static_cast<long>(static_cast<double>(f) * cfg_.high);
    return cfg_.hard > 0 ? static_cast<long>(static_cast<double>(rebase_at()) * cfg_.high / cfg_.hard) : 0;
}

std::string TapeEngine::level(long pool) const {
    if (const long f = fast_layer(); f > 0) {
        const double fill = static_cast<double>(pool) / static_cast<double>(f);
        if (fill >= cfg_.hard) return "hard";
        if (fill >= cfg_.high) return "high";
        if (fill >= cfg_.low) return "low";
        return "ok";
    }
    const long at = rebase_at();
    if (at <= 0) return "ok";
    if (pool >= at) return "hard";
    return pool >= high_at() ? "high" : "ok";
}

void TapeEngine::note(const std::string& s) const {
    if (on_note) on_note(s);
}

void TapeEngine::soak(Item& item) {
    if (item.soaked || item.msg.role != "assistant" || item.msg.reasoning.empty()) return;
    item.soaked = true;
    if (soaker_) soaker_->reasoning(session_->id(), item.msg.reasoning, session_->generation());
}

TapeStats TapeEngine::compute_stats() const {
    TapeStats s;
    s.epoch = session_->generation();
    s.history = session_->items().size();
    s.tape = tape_.messages();
    s.tokens = tokens_;
    s.pool = tokens_;
    s.rebase_at = rebase_at();
    s.high_at = high_at();
    s.fast_layer = fast_layer();
    s.prompt = prompt_;
    s.reused = reused_;
    s.level = level(tokens_);
    s.prep = prep_running_.load() ? "warming" : ready_.load() ? "ready" : "";
    s.server = !ep_.info().local ? "remote"
               : (ep_.info().strata ? "strata ×" : "llama ×") + std::to_string(ep_.info().slots);
    s.visible = deck_.visible() + (deck_.active() ? 3 : 0);
    s.deferred = deck_.deferred();
    s.main_slot = ep_.main_slot();
    s.maze = mem_ && mem_->up();
    s.ledger = ledger_.size();
    s.skills = router_ ? router_->skills() : 0;
    s.steered = steered_;
    s.recall = last_recall_;
    if (soaker_) {
        const auto st = soaker_->stats();
        s.soaked = st.written;
        s.soak_queue = st.queued - st.written - st.spooled;
        s.spooled = st.spooled;
        s.edges = st.edges;
        s.unledgered = st.unledgered;
    }
    s.session = session_->id();
    return s;
}

TapeStats TapeEngine::stats() const {
    const auto r = snap_.read();
    return r ? *r : TapeStats{};
}

void TapeEngine::publish() const { snap_.publish(std::make_unique<const TapeStats>(compute_stats())); }

void TapeEngine::emit() const {
    TapeStats s = compute_stats();
    snap_.publish(std::make_unique<const TapeStats>(s));
    if (on_stats) on_stats(s);
}

void TapeEngine::sync() {
    drain_ledger();
    publish();
}

// ---- the session in the graph ----

void TapeEngine::drain_ledger() {
    if (!soaker_) return;
    LedgerEntry e;
    while (soaker_->take(e))
        if (e.session == session_->id()) ledger_.push_back(std::move(e));
}

std::set<long> TapeEngine::in_window() const {
    std::set<long> ids;
    for (const auto& e : ledger_)
        if (e.epoch == session_->generation()) ids.insert(e.id);
    return ids;
}

std::vector<long> TapeEngine::turn_ids(bool earlier_only) const {
    std::vector<long> ids;
    for (const auto& e : ledger_)
        if (e.kind == "turn" && (!earlier_only || e.epoch != session_->generation())) ids.push_back(e.id);
    return ids;
}

// ---- turn start ----

void TapeEngine::begin_turn(const std::string& user_message) {
    // Strata holds one conversation: main wants it back, a prewarm still reading is closed
    // (its progress survives as checkpoints). A slot server lets it finish beside main.
    if (prep_running_.load() && ep_.single_sequence()) join_prep(true);
    else if (!prep_running_.load() && prep_thread_.joinable()) prep_thread_.join();

    drain_ledger();
    turn_open_ = true;
    question_ = user_message;
    steered_ = 0;   // the count is this turn's, not the session's
    Message um;
    um.role = "user";
    um.content = user_message;
    session_->append(std::move(um));
    injection_.clear();

    const int gen = session_->generation();
    watermark(true);
    const bool rebased = session_->generation() != gen;

    // What to look up for this turn is the router's call: pointers from the catalogues at once, the
    // pod's recall within its deadline. The note is rendered onto the question once and rides the tape
    // from there. A rebase that just recalled for this question already carries the pod's answer.
    records_.clear();
    pending_calls_.clear();
    if (router_) {
        know::TurnStarted t;
        t.text = user_message;
        t.anchored = anchored_query(session_->items(), session_->current_user());
        t.trivial = !cfg_.turn_recall || trivial_prompt(user_message);   // the catalogues need no pod: the router asks it for the deep leg only
        t.skip_deep = rebased;
        t.epoch = session_->generation();
        t.in_window = in_window();
        t.earlier = turn_ids(true);
        if (on_recall) on_recall(true);
        const know::Delivery d = router_->on(t);
        if (on_recall) on_recall(false);
        injection_ = d.note;
        last_recall_ = d.summary;
        if (!d.note.empty() || d.deep_late) {
            Prefetch p = d.deep;
            p.skill = d.skill;
            if (on_prefetch) on_prefetch(p, last_recall_);
            else note("◈ " + last_recall_);
        }
    }
    emit();
}

void TapeEngine::watermark(bool turn_start) {
    // tokens as the server counted the last call, plus what this round's results add: at the ratio the server
    // measured on the tape (digits, escapes and code tokenize worse than prose), never under chars/4
    const long pending = static_cast<long>(static_cast<double>(tool_chars_) / 4.0 * std::max(1.0, render_.ratio()));
    const long pool = ep_.pool_tokens(tokens_ + pending);
    const std::string lvl = level(pool);

    // Past the low watermark an idle sidekick's leftover KV pays no rent.
    if (ep_.multi_slot() && lvl != "ok" && !prep_running_.load() && !ready_.load()) {
        const int side = ep_.sidekick_slot();
        for (const auto& s : ep_.slots())
            if (s.id == side && s.tokens > 0 && !s.busy && ep_.erase(side))
                note("watermark " + lvl + ": erased idle sidekick slot " + std::to_string(side) + " (" +
                     thousands(s.tokens) + " tokens)");
    }
    if (adopt(pool, turn_start)) return;
    if (rebase_at() > 0 && pool >= rebase_at()) rebase(turn_start ? "turn start" : "inside the turn");
}

bool TapeEngine::adopt(long pool, bool turn_start) {
    std::unique_ptr<Prep> p(ready_.exchange(nullptr));
    if (!p) return false;
    if (p->generation != session_->generation()) {
        if (p->slot >= 0) ep_.erase(p->slot);
        note("prewarm discarded: the history changed since it was built");
        return false;
    }
    const bool due = rebase_at() > 0 && pool >= rebase_at();
    // On Strata the warm epoch is what the engine holds now; resuming the old
    // one would re-read it, so the swap is due at once.
    const bool parked = p->warmed && ep_.single_sequence() && turn_start;
    if (!due && !parked) {
        ready_.store(p.release());
        return false;
    }
    const auto& items = session_->items();
    std::vector<ItemPtr> merged = p->base;
    if (p->snapshot_len <= items.size()) merged.insert(merged.end(), items.begin() + static_cast<long>(p->snapshot_len), items.end());
    const size_t before = items.size();
    session_->replace(std::move(merged));
    if (p->slot >= 0 && ep_.multi_slot() && p->slot != ep_.main_slot()) {
        const int old = ep_.main_slot();
        ep_.promote(p->slot);
        const bool erased = ep_.erase(old);
        note("main moved to the prewarmed slot " + std::to_string(p->slot) + (erased ? ", old epoch erased" : ""));
    }
    drop_pending_ = ep_.single_sequence();
    if (router_) router_->on(know::Rebased{session_->generation(), question_});
    note("⟲ rebase (prewarmed" + std::string(p->warmed ? "" : ", not warmed") + "): epoch " +
         std::to_string(session_->generation()) + " · " + thousands(pool) + " tokens · " + std::to_string(before) +
         " → " + std::to_string(session_->items().size()) + " messages");
    tokens_ = 0;
    tool_chars_ = 0;
    return true;
}

bool TapeEngine::rebase(const char* why) {
    if (std::unique_ptr<Prep> stale(ready_.exchange(nullptr)); stale && stale->slot >= 0) ep_.erase(stale->slot);
    bool recalled = false;
    drain_ledger();
    auto base = retrieval_base(session_->items(), session_->id(), &recalled, render_.ratio(), turn_ids(false));
    const size_t before = session_->items().size();
    const long tokens = tokens_;
    // A rebase that cannot make room would rebase again on the next call,
    // dropping the working state each time: the window stays over instead.
    const long after = epoch_tokens(base, render_.ratio());
    if (high_at() > 0 && after >= high_at()) {
        if (skipped_gen_ != session_->generation())
            note(std::string("⟲ rebase skipped (") + why + "): a new epoch would still be ~" + thousands(after) +
                 " tokens (high watermark " + thousands(high_at()) + ") — the window stays over the fast layer");
        skipped_gen_ = session_->generation();
        return false;
    }
    session_->replace(std::move(base));
    drop_pending_ = ep_.single_sequence();
    if (router_) router_->on(know::Rebased{session_->generation(), question_});
    note(std::string("⟲ rebase (") + why + "): epoch " + std::to_string(session_->generation()) + " · " +
         thousands(tokens) + " → ~" + thousands(after) + " tokens · " + std::to_string(before) + " → " +
         std::to_string(session_->items().size()) + " messages · " + (recalled ? "recall in turn 0" : "no recall"));
    tokens_ = 0;
    tool_chars_ = 0;
    return true;
}

// What the server counted is the truth the estimate missed: take it, rebuild the history around it, and let the loop
// send the call again. Without it the next turn starts from the old count and sends the same request.
bool TapeEngine::on_overflow(const std::string& error) {
    long prompt = 0, win = 0;
    if (!parse_overflow(error, &prompt, &win)) return false;
    if (win > 0) window_seen_.store(win, std::memory_order_relaxed);
    if (prompt > 0) tokens_ = prompt;
    tool_chars_ = 0;
    join_prep(true);
    note("the server refused " + (prompt > 0 ? thousands(prompt) + " tokens" : std::string("the request")) +
         (win > 0 ? " for a window of " + thousands(win) : std::string()) + ": rebasing, then sending again");
    const bool made = rebase("window refused");
    emit();
    return made;
}

long TapeEngine::epoch_tokens(const std::vector<ItemPtr>& base, double ratio) const {
    long t = prefix_tokens(ratio);
    for (const auto& it : base) t += static_cast<long>(static_cast<double>(est_tokens(*it)) * ratio);
    return t;
}

long TapeEngine::prefix_tokens(double ratio) const {
    return static_cast<long>(static_cast<double>(system_.size() + core::json_dump(deck_.definitions()).size()) / 4.0 * ratio);
}

std::vector<ItemPtr> TapeEngine::retrieval_base(const std::vector<ItemPtr>& items, const std::string& session_id,
                                                bool* recalled, double ratio, const std::vector<long>& turn_ids) const {
    long cur = -1;
    for (size_t i = items.size(); i-- > 0;)
        if (items[i]->msg.role == "user" && !items[i]->hidden) {
            cur = static_cast<long>(i);
            break;
        }
    const long protect = std::max(2, cfg_.protect_last_n);
    long cut = std::max(0L, static_cast<long>(items.size()) - protect);
    while (cut > 0 && cut < static_cast<long>(items.size()) && items[static_cast<size_t>(cut)]->msg.role == "tool") --cut;
    if (cur >= 0 && cur >= cut) cut = cur + 1;   // the question leads the base; the tail starts after it
    // The new epoch has to land well under the rebase point, or the next call
    // rebases again: the tail gives way from its front, a whole tool group at a
    // time, and what it ran is named in [this turn so far].
    if (const long target = rebase_at() / 2; target > 0) {
        auto est = [&](long from) {
            long t = prefix_tokens(ratio) + 1200;   // + the anchor and its recall
            if (cur >= 0) t += static_cast<long>(static_cast<double>(est_tokens(*items[static_cast<size_t>(cur)])) * ratio);
            for (long i = from; i < static_cast<long>(items.size()); ++i)
                if (i != cur) t += static_cast<long>(static_cast<double>(est_tokens(*items[static_cast<size_t>(i)])) * ratio);
            return t;
        };
        // never past the newest tool group: it is the live state the turn continues from
        long keep = static_cast<long>(items.size());
        for (long i = keep - 1; i > cur; --i)
            if (items[static_cast<size_t>(i)]->msg.role == "assistant" && !items[static_cast<size_t>(i)]->msg.tool_calls.empty()) {
                keep = i;
                break;
            }
        while (cut < keep && est(cut) > target) {
            const Item& head = *items[static_cast<size_t>(cut)];
            ++cut;
            if (head.msg.role == "assistant" && !head.msg.tool_calls.empty())
                while (cut < static_cast<long>(items.size()) && items[static_cast<size_t>(cut)]->msg.role == "tool") ++cut;
        }
    }

    std::string pointer = "[rebased] ariadne rebuilt this conversation to keep the working context inside the GPU's fast "
                         "layer. Everything before this point is in mazemaker under auto:turn:" + session_id + ":*";
    if (!turn_ids.empty()) {   // by id: read back at once, no search
        std::string ids;
        for (size_t i = turn_ids.size() > 12 ? turn_ids.size() - 12 : 0; i < turn_ids.size(); ++i)
            ids += (ids.empty() ? "" : ",") + std::to_string(turn_ids[i]);
        pointer += "; its turns are memory ids [" + ids + "] — mazemaker_get(memory_ids=[" + ids +
                  "]) reads any of them back at once";
    }
    pointer += ". Read it back before redoing any of it.";
    std::vector<std::string> blocks = {pointer};
    if (cur >= 0) {
        const std::vector<ItemPtr> dropped(items.begin() + cur + 1, items.begin() + std::max(cur + 1, cut));
        if (std::string t = turn_so_far(dropped); !t.empty()) blocks.push_back(t);
    }
    std::string block;
    if (mem_ && mem_->up() && cur >= 0) {
        const std::string query = anchored_query(items, cur);
        for (int attempt = 0; attempt < 2 && block.empty(); ++attempt)   // the whole epoch leaves the window: nothing is in it
            block = (router_ ? router_->recall_now(query, {}, turn_ids) : recaller_.run(query, {}, turn_ids)).block;
    }
    if (recalled) *recalled = !block.empty();
    if (router_)   // the skills the model was working from: their text left with the epoch
        if (const std::string r = router_->reattach(); !r.empty()) blocks.push_back(r);
    if (!block.empty()) {
        blocks.push_back(block);
    } else if (mem_ && mem_->up()) {
        blocks.push_back("[recall required] The rebase returned no prefetched memories. Do NOT assume this "
                         "conversation started here. Call mazemaker_recall on the question above BEFORE answering, "
                         "re-reading files, or redoing any prior work.");
    } else {
        blocks.push_back("[memory unreachable] mazemaker did not answer, so nothing could be recalled: the turns "
                         "before this point are not in this window. Re-read the files or ask before assuming what "
                         "was decided.");
    }

    std::vector<ItemPtr> base;
    if (cur >= 0) base.push_back(items[static_cast<size_t>(cur)]);
    auto anchor = std::make_shared<Item>();
    anchor->msg.role = "user";
    for (size_t i = 0; i < blocks.size(); ++i) anchor->msg.content += (i ? "\n\n" : "") + blocks[i];
    anchor->hidden = true;
    base.push_back(anchor);
    for (long i = std::max(0L, cut); i < static_cast<long>(items.size()); ++i)
        if (i != cur) base.push_back(items[static_cast<size_t>(i)]);
    return base;
}

// ---- every call ----

std::vector<Message> TapeEngine::payload() {
    const auto& items = session_->items();
    const long cur = session_->current_user();
    const ItemPtr current = cur >= 0 ? items[static_cast<size_t>(cur)] : nullptr;
    const int gen = session_->generation();
    std::vector<Message> out;
    if (!system_.empty()) {
        Message sys;
        sys.role = "system";
        sys.content = system_;
        out.push_back(std::move(sys));
    }
    tool_chars_ = 0;

    if (cfg_.tape && tape_) {
        const long start = index_of(items, tape_.first_source());
        if (start >= 0) {
            const std::vector<ItemPtr> view(items.begin() + start, items.end());
            const auto [blocks, covered] = tape_.matched(view, gen);
            if (blocks) {
                tape_.truncate(blocks);
                std::vector<Message> body = tape_.payload();
                for (size_t i = covered; i < view.size(); ++i) {
                    auto r = render_.one(*view[i], view[i] == current, injection_, view);
                    body.insert(body.end(), r.begin(), r.end());
                    tape_.append(view[i], std::move(r));
                }
                body = Renderer::sanitize(std::move(body));
                out.insert(out.end(), std::make_move_iterator(body.begin()), std::make_move_iterator(body.end()));
                return out;
            }
        }
        tape_.reset();   // the history under the tape was replaced: a new epoch
    }
    auto ep = render_.epoch(items, cur, injection_);
    if (cfg_.tape) tape_.start(ep.sources, ep.rendered, gen);
    out.insert(out.end(), std::make_move_iterator(ep.rendered.begin()), std::make_move_iterator(ep.rendered.end()));
    emit();
    return out;
}

void TapeEngine::on_call(const agent::CallStats& st) {
    if (drop_pending_ && st.prompt_tokens > 0) {   // the new epoch answered: the old one is dead weight
        drop_pending_ = false;
        ep_.drop_parked();
        note("⟲ old epoch dropped from the engine's conversation cache");
    }
    if (st.prompt_tokens > 0) {
        tokens_ = st.prompt_tokens + st.completion_tokens;
        prompt_ = st.prompt_tokens;
        reused_ = st.cached_tokens;
        // the server counts the tools array too: so must the estimate it is compared to
        size_t chars = system_.size() + core::json_dump(deck_.definitions()).size();
        for (const auto& m : tape_.payload()) {
            chars += m.content.size() + m.reasoning.size() + 16;
            for (const auto& tc : m.tool_calls) chars += tc.name.size() + tc.arguments.size() + 24;
        }
        render_.note_ratio(st.prompt_tokens, static_cast<long>(chars / 4));
    }
    emit();
}

void TapeEngine::on_assistant(const Message& m) {
    session_->append(m);
    pending_calls_ = m.tool_calls;   // each meets its result in on_tool_results
}

void TapeEngine::on_tool_results(const std::vector<Message>& results) {
    for (const auto& r : results) {
        tool_chars_ += r.content.size();
        session_->append(r);
        if (router_)
            for (const auto& c : pending_calls_)
                if (c.id == r.tool_call_id) {
                    records_.push_back(know::make_record(c, r.content));
                    break;
                }
    }
}

// Mid-turn input. A real user item, not a hidden note: the model asked it, the
// operator wrote it during the turn. `steered` is what separates it from the turn's
// question in the soak and the rebase anchor, and Session::current_user() moves to
// it — the newest question is the one the next call answers.
void TapeEngine::on_steering(const std::vector<Message>& notes) {
    for (const auto& m : notes) {
        session_->append(m)->steered = true;
        steered_ += 1;
    }
}

void TapeEngine::append_note(const std::string& text) {
    Message m;
    m.role = "user";
    m.content = text;
    session_->append(std::move(m), true)->note = true;
    tool_chars_ += text.size();   // the next watermark counts it
}

void TapeEngine::speak_after_tools() {
    if (!router_) return;
    know::CallsFinished f;
    f.calls = std::move(records_);
    records_.clear();
    const know::Delivery d = router_->on(f);
    if (!d.note.empty()) {
        append_note(d.note);
        note("◈ " + d.summary);
    }
    if (const auto late = router_->late()) {   // the pod's recall for this turn, after its deadline
        append_note(late->note);
        note("◈ " + late->summary);
    }
}

void TapeEngine::after_tools() {
    watermark(false);
    speak_after_tools();
    emit();
}

// ---- turn end ----

void TapeEngine::end_turn(const std::string& final_response, bool interrupted) {
    if (!turn_open_) return;   // never begun, or already ended: nothing to close twice
    turn_open_ = false;
    if (router_) router_->end_turn();
    if (!interrupted && !final_response.empty() && soaker_)
        soaker_->turn(session_->id(), question_, final_response, session_->generation());
    // the final answer's thinking is never sent again this turn: soak it now
    const auto& items = session_->items();
    if (!items.empty() && items.back()->msg.role == "assistant") soak(*items.back());
    if (!interrupted && !final_response.empty()) start_prep();
    emit();
}

void TapeEngine::start_prep() {
    if (!cfg_.prewarm || prep_running_.load() || ready_.load() || !mem_ || !mem_->up()) return;
    const ServerInfo& si = ep_.info();
    // a single-slot llama-server would evict main to warm it; a remote API has nothing to warm
    if (!si.local || (si.slots < 2 && !ep_.single_sequence())) return;
    const long pool = ep_.pool_tokens(tokens_);
    if (high_at() <= 0 || pool < high_at()) return;
    if (prep_thread_.joinable()) prep_thread_.join();

    const std::vector<ItemPtr> snapshot = session_->items();
    const int gen = session_->generation();
    const std::string sid = session_->id();
    const int slot = ep_.multi_slot() ? ep_.sidekick_slot() : -1;
    Renderer quiet = render_.quiet();
    const double ratio = render_.ratio();
    drain_ledger();
    const std::vector<long> ids = turn_ids(false);   // the thread gets a copy, never the ledger
    prep_cancel_.store(false);
    prep_running_.store(true);
    note("⟲ prewarming the next epoch: " + thousands(pool) + " tokens ≥ high watermark " + thousands(high_at()) +
         (slot >= 0 ? " · sidekick slot " + std::to_string(slot) : " · parked (one-conversation engine)"));
    prep_thread_ = std::thread([this, snapshot, gen, sid, slot, quiet, ratio, ids]() mutable {
        auto p = std::make_unique<Prep>();
        bool recalled = false;
        p->base = retrieval_base(snapshot, sid, &recalled, ratio, ids);
        if (high_at() > 0 && epoch_tokens(p->base, ratio) >= high_at()) {   // no room to make: nothing to warm
            note("⟲ prewarm dropped: the next epoch would not land under the high watermark");
            prep_running_.store(false);
            return;
        }
        p->snapshot_len = snapshot.size();
        p->generation = gen;
        p->slot = slot;
        if (!prep_cancel_.load()) {
            // exactly what main sends at the next turn start: the system prompt,
            // the tools array, the epoch render (the question is not current then)
            const auto ep = quiet.epoch(p->base, p->base.empty() || p->base[0]->hidden ? -1 : 0, "");
            std::vector<Message> msgs;
            if (!system_.empty()) {
                Message sys;
                sys.role = "system";
                sys.content = system_;
                msgs.push_back(std::move(sys));
            }
            msgs.insert(msgs.end(), ep.rendered.begin(), ep.rendered.end());
            core::Json body = llm_.body(msgs, deck_.definitions());
            if (slot >= 0) core::obj_set(body, "id_slot", core::json_num(slot));
            p->warmed = llm_.prime(std::move(body), &prep_cancel_, 900000);
        }
        if (!prep_cancel_.load()) {
            note(std::string("⟲ next epoch ") + (p->warmed ? "prewarmed" : "built (warming failed)") + ": " +
                 std::to_string(snapshot.size()) + " → " + std::to_string(p->base.size()) + " messages" +
                 (recalled ? " · recall in" : ""));
            delete ready_.exchange(p.release());
        }
        prep_running_.store(false);
    });
}

void TapeEngine::join_prep(bool cancel) {
    if (cancel) prep_cancel_.store(true);
    if (prep_thread_.joinable()) prep_thread_.join();
}

void TapeEngine::reset() {
    join_prep(true);
    delete ready_.exchange(nullptr);
    turn_open_ = false;
    spill_->remove_all();
    session_ = std::make_unique<Session>(Session::new_id());
    spill_ = std::make_unique<Spill>(spill_root_ + "/" + session_->id());
    render_ = Renderer(cfg_.render, [m = mem_] { return m && m->up(); }, [this](Item& it) { soak(it); }, spill_.get());
    ledger_.clear();
    last_recall_.clear();
    tape_.reset();
    injection_.clear();
    question_.clear();
    tokens_ = prompt_ = reused_ = 0;
    tool_chars_ = 0;
    if (router_) router_->on(know::SessionStarted{});
    emit();
}

std::vector<TapeEngine::SessionRef> TapeEngine::recent_sessions(const std::string& prefix, int limit) const {
    std::vector<SessionRef> out;
    if (!mem_ || !mem_->configured()) return out;
    std::vector<Hit> rows;
    for (int off = 0; off < 5000; off += 500) {
        auto page = mem_->labels("auto:turn:" + prefix, off, 500);
        rows.insert(rows.end(), page.begin(), page.end());
        if (page.size() < 500) break;
    }
    for (const auto& r : rows) {   // auto:turn:<session>:<hex ts>
        const std::string rest = r.label.substr(10);
        const size_t c = rest.rfind(':');
        if (c == std::string::npos) continue;
        const std::string sid = rest.substr(0, c), ts = rest.substr(c + 1);
        auto it = std::find_if(out.begin(), out.end(), [&](const SessionRef& s) { return s.id == sid; });
        if (it == out.end()) out.push_back(SessionRef{sid, ts, 1});
        else {
            ++it->turns;
            if (ts > it->last) it->last = ts;
        }
    }
    std::sort(out.begin(), out.end(), [](const SessionRef& a, const SessionRef& b) {
        return a.last.size() != b.last.size() ? a.last.size() > b.last.size() : a.last > b.last;
    });
    if (static_cast<int>(out.size()) > limit) out.resize(static_cast<size_t>(limit));
    for (auto& s : out) {   // hex unix seconds -> UTC
        const std::time_t t = static_cast<std::time_t>(std::strtoll(s.last.c_str(), nullptr, 16));
        std::tm tm{};
        gmtime_r(&t, &tm);
        char b[32];
        std::strftime(b, sizeof b, "%Y-%m-%d %H:%M UTC", &tm);
        s.last = b;
    }
    return out;
}

bool TapeEngine::resume(const std::string& session_id, std::string* why) {
    if (!mem_ || !mem_->configured()) {
        if (why) *why = "no mazemaker";
        return false;
    }
    std::vector<Hit> rows;
    for (int off = 0; off < 5000; off += 500) {
        auto page = mem_->labels("auto:turn:" + session_id + ":", off, 500);
        rows.insert(rows.end(), page.begin(), page.end());
        if (page.size() < 500) break;
    }
    if (rows.empty()) {
        if (why) *why = "no auto:turn:" + session_id + ":* in the maze";
        return false;
    }
    std::vector<long> ids;
    for (const auto& r : rows) ids.push_back(r.id);
    auto bodies = mem_->get(ids);
    std::sort(bodies.begin(), bodies.end(), [](const Hit& a, const Hit& b) {
        return a.label.size() != b.label.size() ? a.label.size() < b.label.size() : a.label < b.label;
    });
    join_prep(true);
    delete ready_.exchange(nullptr);
    turn_open_ = false;
    spill_->remove_all();
    session_ = std::make_unique<Session>(session_id);
    spill_ = std::make_unique<Spill>(spill_root_ + "/" + session_->id());
    render_ = Renderer(cfg_.render, [m = mem_] { return m && m->up(); }, [this](Item& it) { soak(it); }, spill_.get());
    tape_.reset();
    ledger_.clear();
    injection_.clear();
    question_.clear();
    tokens_ = prompt_ = reused_ = 0;
    tool_chars_ = 0;
    if (router_) router_->on(know::SessionStarted{});
    std::string last;
    for (const auto& b : bodies) {   // "session:… @ …\n\n=== USER ===\n<u>\n\n=== ASSISTANT ===\n<a>"
        static const std::string kUser = "=== USER ===\n", kAsst = "\n\n=== ASSISTANT ===\n";
        const size_t u = b.content.find(kUser), a = b.content.find(kAsst);
        if (u == std::string::npos || a == std::string::npos || a < u) continue;
        Message um, am;
        um.role = "user";
        um.content = b.content.substr(u + kUser.size(), a - u - kUser.size());
        am.role = "assistant";
        am.content = b.content.substr(a + kAsst.size());
        session_->append(std::move(um));
        session_->append(std::move(am));
        ledger_.push_back(LedgerEntry{"turn", b.label, session_id, b.id, session_->generation()});
        last = b.label;
    }
    if (soaker_ && !last.empty()) soaker_->continue_chain(last);
    note("⟲ resumed " + session_id + " from the maze: " + std::to_string(ledger_.size()) + " turns");
    emit();
    return true;
}

void TapeEngine::rebase_now() {
    join_prep(true);
    rebase("by hand");
    emit();
}

}  // namespace context
}  // namespace dd
