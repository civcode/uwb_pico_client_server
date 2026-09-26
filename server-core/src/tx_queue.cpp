#include "uwb/server/tx_queue.hpp"

namespace uwb::server {

ConnectionTxQueue::ConnectionTxQueue(std::uint32_t highCapacity, std::uint32_t normalCapacity) noexcept
    : highCapacity_(highCapacity), normalCapacity_(normalCapacity) {}

void ConnectionTxQueue::dropOldest(std::deque<Entry> &queue) {
    if (queue.empty()) {
        return;
    }
    queue.pop_front();
    ++dropped_;
}

PushOutcome ConnectionTxQueue::push(ConstBytes frame, TxPriority priority) {
    if (priority == TxPriority::High) {
        // Control/ACK traffic must be delivered. A full high-priority queue means
        // the peer is not draining: the caller treats this as a connection-level
        // error instead of silently dropping a required response.
        if (high_.size() >= highCapacity_) {
            return PushOutcome::RejectedFull;
        }
        high_.push_back(Entry{ByteBuffer(frame.begin(), frame.end()), priority});
        return PushOutcome::Accepted;
    }

    if (normal_.size() >= normalCapacity_) {
        dropOldest(normal_);
        normal_.push_back(Entry{ByteBuffer(frame.begin(), frame.end()), priority});
        return PushOutcome::DroppedOldest;
    }
    normal_.push_back(Entry{ByteBuffer(frame.begin(), frame.end()), priority});
    return PushOutcome::Accepted;
}

std::optional<ConnectionTxQueue::Selection> ConnectionTxQueue::selectNext() noexcept {
    if (!high_.empty()) {
        return Selection{&high_, 0};
    }
    // Drain Normal before Low, FIFO within a class (docs/protocol_decisions.md §13).
    for (std::size_t i = 0; i < normal_.size(); ++i) {
        if (normal_[i].priority == TxPriority::Normal) {
            return Selection{&normal_, i};
        }
    }
    if (!normal_.empty()) {
        return Selection{&normal_, 0};
    }
    return std::nullopt;
}

std::optional<ByteBuffer> ConnectionTxQueue::popNext() {
    auto selection = selectNext();
    if (!selection) {
        return std::nullopt;
    }
    std::deque<Entry> &queue = *selection->queue;
    ByteBuffer frame = std::move(queue[selection->index].frame);
    queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(selection->index));
    return frame;
}

const ByteBuffer *ConnectionTxQueue::peekNext() noexcept {
    auto selection = selectNext();
    if (!selection) {
        return nullptr;
    }
    return &(*selection->queue)[selection->index].frame;
}

void ConnectionTxQueue::popFront() noexcept {
    auto selection = selectNext();
    if (!selection) {
        return;
    }
    std::deque<Entry> &queue = *selection->queue;
    queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(selection->index));
}

std::size_t ConnectionTxQueue::pending(TxQueueClass queueClass) const noexcept {
    return queueClass == TxQueueClass::High ? high_.size() : normal_.size();
}

void ConnectionTxQueue::clear() noexcept {
    high_.clear();
    normal_.clear();
}

std::uint32_t ConnectionTxQueue::capacity(TxQueueClass queueClass) const noexcept {
    return queueClass == TxQueueClass::High ? highCapacity_ : normalCapacity_;
}

} // namespace uwb::server
