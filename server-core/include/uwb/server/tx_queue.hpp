#pragma once

#include <cstdint>
#include <deque>
#include <optional>

#include "uwb/protocol/bytes.hpp"

namespace uwb::server {

using uwb::protocol::ByteBuffer;
using uwb::protocol::ConstBytes;

// TX priority classes (specification §38).
enum class TxPriority : std::uint8_t {
    High = 0, // service responses, ACKs, connection management, Stream Status, Alive
    Normal = 1, // non-critical status events
    Low = 2,    // streaming samples and bulk results (§38 class 3)
};

// Physical queue classes (specification §53 defines one high-priority and one
// normal-priority queue per connection). Low-priority frames live in the
// normal queue and are drained last (docs/protocol_decisions.md §13).
enum class TxQueueClass : std::uint8_t {
    High = 0,
    Normal = 1,
};

[[nodiscard]] constexpr TxQueueClass queueClassOf(TxPriority priority) noexcept {
    return priority == TxPriority::High ? TxQueueClass::High : TxQueueClass::Normal;
}

enum class PushOutcome : std::uint8_t {
    Accepted = 0,
    DroppedOldest = 1, // bounded queue overflow: oldest frame sacrificed
    RejectedFull = 2,  // nothing queued
};

// Bounded per-connection TX scheduler (specification §38, §53).
class ConnectionTxQueue {
public:
    ConnectionTxQueue(std::uint32_t highCapacity, std::uint32_t normalCapacity) noexcept;

    // Queue one already-encoded frame (generic header + payload).
    [[nodiscard]] PushOutcome push(ConstBytes frame, TxPriority priority);

    // Next frame to transmit: highest priority first, Low drained after Normal.
    [[nodiscard]] std::optional<ByteBuffer> popNext();

    // Look at the next frame without removing it, so a transport that cannot
    // accept a frame now does not lose queued traffic (specification §38).
    [[nodiscard]] const ByteBuffer *peekNext() noexcept;
    void popFront() noexcept;

    [[nodiscard]] bool empty() const noexcept { return high_.empty() && normal_.empty(); }
    [[nodiscard]] std::size_t pending() const noexcept { return high_.size() + normal_.size(); }
    [[nodiscard]] std::size_t pending(TxQueueClass queueClass) const noexcept;

    [[nodiscard]] std::uint32_t droppedCount() const noexcept { return dropped_; }
    void clear() noexcept;

    [[nodiscard]] std::uint32_t capacity(TxQueueClass queueClass) const noexcept;

private:
    struct Entry {
        ByteBuffer frame;
        TxPriority priority = TxPriority::High;
    };

    void dropOldest(std::deque<Entry> &queue);
    struct Selection {
        std::deque<Entry> *queue;
        std::size_t index;
    };
    [[nodiscard]] std::optional<Selection> selectNext() noexcept;

    std::deque<Entry> high_;
    std::deque<Entry> normal_;
    std::uint32_t highCapacity_;
    std::uint32_t normalCapacity_;
    std::uint32_t dropped_ = 0;
};

} // namespace uwb::server
