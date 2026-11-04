// src/context/plan.cpp — impl of dd/context/plan.hpp.
#include "dd/context/plan.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <utility>

namespace dd {
namespace context {

const char* const kPlanNamespace = "ariadne:plan:";

namespace {

std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

std::string iso_utc(long long unix_s) {
    const std::time_t t = static_cast<std::time_t>(unix_s);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return b;
}

// "2026-11-04T03:40:24Z" -> seconds; 0 when the text is not that shape.
long long unix_of_iso(const std::string& s) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    if (std::sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &sec) != 6) return 0;
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = sec;
    return static_cast<long long>(timegm(&tm));
}

bool valid_status(const std::string& s) {
    return s == "pending" || s == "in_progress" || s == "completed" || s == "cancelled";
}

std::string marker_of(const std::string& status) {
    if (status == "completed") return "[x]";
    if (status == "in_progress") return "[>]";
    if (status == "cancelled") return "[~]";
    return "[ ]";
}

// One ledger line: "- [>] t2. text (in_progress)". The marker is decoration, the id ends at
// the first '.' or ' ', the status is the trailing parenthesised word when it is a status the
// tool would accept (content may end in parentheses of its own).
bool parse_item_line(const std::string& line, tools::TodoItem* out) {
    if (line.size() < 5 || line[0] != '-' || line[1] != ' ' || line[2] != '[') return false;
    size_t pos = line.find(']');
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < line.size() && line[pos] == ' ') ++pos;
    const size_t id_end = line.find_first_of(". ", pos);
    if (id_end == std::string::npos || id_end <= pos) return false;
    tools::TodoItem it;
    it.id = line.substr(pos, id_end - pos);
    size_t b = id_end;
    while (b < line.size() && (line[b] == '.' || line[b] == ' ')) ++b;
    std::string rest = line.substr(b);
    it.status = "pending";
    const size_t paren = rest.rfind(" (");
    if (paren != std::string::npos && rest.back() == ')') {
        const std::string word = rest.substr(paren + 2, rest.size() - paren - 3);
        if (valid_status(word)) {
            it.status = word;
            rest = rest.substr(0, paren);
        }
    }
    it.content = trim(rest);
    if (it.content.empty()) it.content = "(no description)";
    if (it.id.empty()) return false;
    *out = std::move(it);
    return true;
}

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        const size_t nl = s.find('\n', pos);
        out.push_back(nl == std::string::npos ? s.substr(pos) : s.substr(pos, nl - pos));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return out;
}

// The state a write is compared against: the items as the node renders them, so two ledgers
// that differ only in an item's id case are the same state and are not written twice.
std::string state_key(const std::vector<tools::TodoItem>& items) {
    std::string k;
    for (const auto& it : items)
        k += "- " + marker_of(it.status) + " " + it.id + ". " + it.content + " (" + it.status + ")\n";
    return k;
}

// Sessions are dd-<hex> (or a Python Daedalus id): no ':' in either, so the revision is the
// last field of the label and everything between the namespace and it is the session.
bool label_parts(const std::string& label, std::string* session, int* rev) {
    const std::string ns = kPlanNamespace;
    if (label.compare(0, ns.size(), ns) != 0) return false;
    const size_t colon = label.rfind(':');
    if (colon == std::string::npos || colon < ns.size()) return false;
    const std::string num = label.substr(colon + 1);
    if (num.empty() || num.find_first_not_of("0123456789") != std::string::npos) return false;
    *session = label.substr(ns.size(), colon - ns.size());
    *rev = std::atoi(num.c_str());
    return !session->empty();
}

// The highest revision of each session in the namespace, as (rev, id).
void newest_per_session(Memory& mem, std::map<std::string, std::pair<int, long>>* out) {
    std::map<std::string, std::pair<int, long>> best;
    for (int off = 0; off < 4000; off += 200) {
        const auto page = mem.labels(kPlanNamespace, off, 200);
        for (const auto& h : page) {
            std::string session;
            int rev = 0;
            if (!label_parts(h.label, &session, &rev)) continue;
            auto it = best.find(session);
            if (it == best.end() || rev > it->second.first) best[session] = {rev, h.id};
        }
        if (page.size() < 200) break;
    }
    *out = std::move(best);
}

}  // namespace

int plan_open(const std::vector<tools::TodoItem>& items) {
    int n = 0;
    for (const auto& it : items)
        if (it.status == "pending" || it.status == "in_progress") ++n;
    return n;
}

std::string plan_label(const std::string& session, int rev) {
    char num[16];
    std::snprintf(num, sizeof num, "%06d", rev);
    return std::string(kPlanNamespace) + session + ":" + num;
}

bool plan_label_split(const std::string& label, std::string* session, int* rev) {
    return label_parts(label, session, rev);
}

std::string plan_content(const PlanDoc& d) {
    const int open = plan_open(d.items);
    std::string s = "ariadne plan: session " + d.session + ", revision " + std::to_string(d.rev) + ", state " +
                    (d.done ? "done" : "open") + ", " + std::to_string(open) + " of " + std::to_string(d.items.size()) +
                    " open, recorded " + iso_utc(d.unix_s) + "\n";
    s += "ledger: " + plan_label(d.session, d.rev) +
         " -- a higher revision of the same session is the newer list; this node is the state\n";
    s += "below. The ledger is the model's own work list: items are [ ] pending, [>] in progress, [x] done,"
         " [~] dropped.\n";
    if (d.items.empty())
        return s + "items: none -- the ledger was emptied: nothing is open and nothing was left in it.\n";
    std::string working;
    for (const auto& it : d.items)
        if (it.status == "in_progress") working += (working.empty() ? "" : ", ") + it.id + ". " + it.content;
    s += "now: " + (working.empty() ? std::string("no item is in progress") : working) + "\n";
    s += "items (" + std::to_string(d.items.size()) + "):\n";
    for (const auto& it : d.items)
        s += "- " + marker_of(it.status) + " " + it.id + ". " + it.content + " (" + it.status + ")\n";
    return s;
}

bool plan_parse(const std::string& content, PlanDoc* out) {
    PlanDoc d;
    bool headed = false;
    for (const auto& line : split_lines(content)) {
        if (!headed && line.rfind("ariadne plan: session ", 0) == 0) {
            const std::string body = line.substr(strlen("ariadne plan: session "));
            const size_t rev_at = body.find(", revision ");
            if (rev_at == std::string::npos) return false;
            d.session = body.substr(0, rev_at);
            d.rev = std::atoi(body.c_str() + rev_at + strlen(", revision "));
            d.done = body.find(", state done") != std::string::npos;
            const size_t rec = body.find(", recorded ");
            if (rec != std::string::npos) d.unix_s = unix_of_iso(body.substr(rec + strlen(", recorded ")));
            headed = true;
            continue;
        }
        tools::TodoItem it;
        if (parse_item_line(line, &it)) d.items.push_back(std::move(it));
    }
    if (!headed || d.session.empty()) return false;
    *out = std::move(d);
    return true;
}

bool plan_parse_items(const std::string& text, std::vector<tools::TodoItem>* out) {
    std::vector<tools::TodoItem> items;
    for (const auto& line : split_lines(text)) {
        tools::TodoItem it;
        if (parse_item_line(line, &it)) items.push_back(std::move(it));
    }
    *out = std::move(items);
    return !out->empty();
}

std::string plan_open_text(const std::string& ledger_text, size_t max_chars) {
    std::string out;
    for (const auto& line : split_lines(ledger_text)) {
        tools::TodoItem it;
        if (!parse_item_line(line, &it)) continue;
        if (it.status != "pending" && it.status != "in_progress") continue;
        const std::string part = it.id + ". " + it.content + (it.status == "in_progress" ? " (in progress)" : "");
        if (out.size() + part.size() + 2 > max_chars) break;
        out += (out.empty() ? "" : "; ") + part;
    }
    return out;
}

void PlanLog::session(const std::string& id) {
    if (session_ == id) return;
    session_ = id;
    rev_ = 0;
    last_.clear();   // the label is the session's: the same list is a new node under a new one
}

void PlanLog::adopt(const PlanDoc& d) {
    rev_ = d.rev;
    last_ = state_key(d.items);
}

bool PlanLog::moved(const std::vector<tools::TodoItem>& items, long long now_s, PlanDoc* d) {
    const std::string key = state_key(items);
    if (key == last_) return false;
    if (key.empty() && session_.empty()) return false;   // never a plan, never a node
    if (key.empty() && last_.empty()) return false;      // nothing was written, nothing to close
    ++rev_;
    PlanDoc doc;
    doc.session = session_;
    doc.rev = rev_;
    doc.unix_s = now_s;
    doc.done = plan_open(items) == 0;
    doc.items = items;
    last_ = key;
    *d = std::move(doc);
    return true;
}

bool plan_of(Memory& mem, const std::string& session, PlanDoc* out) {
    if (session.empty() || !mem.configured()) return false;
    const std::string prefix = std::string(kPlanNamespace) + session + ":";
    int best_rev = -1;
    long best_id = 0;
    for (int off = 0; off < 1000; off += 200) {
        const auto page = mem.labels(prefix, off, 200);
        for (const auto& h : page) {
            std::string got;
            int rev = 0;
            if (!label_parts(h.label, &got, &rev)) continue;
            if (rev > best_rev) {
                best_rev = rev;
                best_id = h.id;
            }
        }
        if (page.size() < 200) break;
    }
    if (best_id <= 0) return false;
    const auto rows = mem.get({best_id});
    for (const auto& h : rows) {
        if (h.id != best_id) continue;
        PlanDoc doc;
        if (!plan_parse(h.content, &doc)) return false;
        doc.id = h.id;
        if (doc.session.empty()) doc.session = session;
        *out = std::move(doc);
        return true;
    }
    return false;
}

bool plan_newest_open(Memory& mem, PlanDoc* out) {
    if (!mem.configured()) return false;
    std::map<std::string, std::pair<int, long>> newest;
    newest_per_session(mem, &newest);
    if (newest.empty()) return false;
    std::vector<long> ids;
    for (const auto& kv : newest) ids.push_back(kv.second.second);
    if (ids.size() > 64) ids.resize(64);   // bounded: the namespace is small, and a boot may not wait
    PlanDoc best;
    for (const auto& h : mem.get(ids)) {
        PlanDoc doc;
        if (!plan_parse(h.content, &doc)) continue;
        if (doc.done || plan_open(doc.items) == 0) continue;
        doc.id = h.id;
        if (doc.unix_s >= best.unix_s) best = std::move(doc);
    }
    if (best.session.empty()) return false;
    *out = std::move(best);
    return true;
}

}  // namespace context
}  // namespace dd
