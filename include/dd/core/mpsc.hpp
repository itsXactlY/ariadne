#pragma once
// dd/core/mpsc.hpp — lock-free hand-off between threads.

#include <atomic>
#include <utility>

namespace dd {
namespace core {

// Vyukov's intrusive MPSC queue: push is one atomic exchange plus a store
// (wait-free, any thread); pop is the single consumer's. A pop that meets a
// producer between its two steps returns false and the item comes next time.
template <class T>
class MpscQueue {
public:
    MpscQueue() : head_(&stub_), tail_(&stub_) {}
    ~MpscQueue() {
        T v;
        while (pop(v)) {}
    }
    MpscQueue(const MpscQueue&) = delete;
    MpscQueue& operator=(const MpscQueue&) = delete;

    void push(T v) {
        Node* n = new Node;
        n->value = std::move(v);
        link(n);
    }

    bool pop(T& out) {
        Node* tail = tail_;
        Node* next = tail->next.load(std::memory_order_acquire);
        if (tail == &stub_) {
            if (!next) return false;
            tail_ = next;
            tail = next;
            next = next->next.load(std::memory_order_acquire);
        }
        if (!next) {
            if (tail != head_.load(std::memory_order_acquire)) return false;
            link(&stub_);
            next = tail->next.load(std::memory_order_acquire);
            if (!next) return false;
        }
        tail_ = next;
        out = std::move(tail->value);
        delete tail;
        return true;
    }

private:
    struct Node {
        std::atomic<Node*> next{nullptr};
        T value{};
    };
    void link(Node* n) {
        n->next.store(nullptr, std::memory_order_relaxed);
        Node* prev = head_.exchange(n, std::memory_order_acq_rel);
        prev->next.store(n, std::memory_order_release);
    }
    Node stub_;
    std::atomic<Node*> head_;
    Node* tail_;
};

}  // namespace core
}  // namespace dd
