#pragma once
// dd/context/memory.hpp — what the harness asks of its memory graph.
//
// The tape forgets on purpose; this is where everything it drops lives, and
// where the session itself is written: each turn and reasoning chain as a
// node, chained by edges, addressable by id. One embedding recall per turn is
// the expensive call (the pod serialises them); the graph walk, primary-key
// reads and facts around it are cheap, so the harness spends its budget on
// one recall and enriches it from the graph. Maze (dd/context/maze.hpp) is
// the mazemaker pod over MCP; tests bring their own.

#include <string>
#include <vector>

namespace dd {
namespace context {

struct Hit {
    long id = 0;
    std::string label, content;
    double score = 0;   // similarity, activation or salience, by call
};

struct Edge {
    std::string a, b;   // labels: the graph resolves them to ids
    double weight = 0.5;
    std::string type;
};

class Memory {
public:
    virtual ~Memory() = default;
    virtual bool configured() const = 0;
    virtual bool up() const = 0;
    virtual bool probe() = 0;

    // The one embedding search per turn. scope: a label glob ("skill:*"), "" for all.
    virtual std::vector<Hit> recall(const std::string& query, int limit, const std::string& scope = "") = 0;
    virtual long remember(const std::string& label, const std::string& content) = 0;   // the new id, -1 on failure
    virtual std::vector<Hit> think(long id, int depth) = 0;          // spreading activation: ids + labels
    virtual std::vector<Hit> get(const std::vector<long>& ids) = 0; // primary-key reads, with content
    virtual std::vector<Hit> facts(long source_id, int limit) = 0;  // atomic facts extracted from a memory
    virtual std::string intent(const std::string& query) = 0;       // preference|temporal|factual|general, "" unknown
    virtual std::vector<Hit> labels(const std::string& prefix, int offset, int limit) = 0;   // ids + labels
    virtual int connect(const std::vector<Edge>& edges) = 0;         // edges written
};

}  // namespace context
}  // namespace dd
