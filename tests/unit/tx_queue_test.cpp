#include <catch2/catch_all.hpp>

#include "uwb/protocol/bytes.hpp"
#include "uwb/server/tx_queue.hpp"

#include "test_helpers.hpp"

using namespace uwb::server;
using uwb::protocol::ByteBuffer;

namespace {

ByteBuffer frameOf(std::uint8_t marker) {
    return ByteBuffer{marker, static_cast<std::uint8_t>(marker + 1), static_cast<std::uint8_t>(marker + 2)};
}

} // namespace

TEST_CASE("High priority drains before Normal, Normal before Low", "[unit][server][queue]") {
    ConnectionTxQueue tx(8, 8);

    CHECK(tx.push(uwb::protocol::bytesOf(frameOf(1)), TxPriority::Normal) == PushOutcome::Accepted);
    CHECK(tx.push(uwb::protocol::bytesOf(frameOf(2)), TxPriority::Low) == PushOutcome::Accepted);
    CHECK(tx.push(uwb::protocol::bytesOf(frameOf(3)), TxPriority::High) == PushOutcome::Accepted);
    CHECK(tx.push(uwb::protocol::bytesOf(frameOf(4)), TxPriority::Normal) == PushOutcome::Accepted);

    const auto first = tx.popNext();
    const auto second = tx.popNext();
    const auto third = tx.popNext();
    const auto fourth = tx.popNext();

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(third.has_value());
    REQUIRE(fourth.has_value());
    CHECK(first->front() == 3); // High first
    CHECK(second->front() == 1); // Normal in FIFO order
    CHECK(third->front() == 4);
    CHECK(fourth->front() == 2); // Low drained last (§53)
}

TEST_CASE("high-priority queue overflow rejects instead of dropping", "[unit][server][queue]") {
    ConnectionTxQueue tx(2, 8);

    CHECK(tx.push(uwb::protocol::bytesOf(frameOf(1)), TxPriority::High) == PushOutcome::Accepted);
    CHECK(tx.push(uwb::protocol::bytesOf(frameOf(2)), TxPriority::High) == PushOutcome::Accepted);
    // §53: control responses and ACKs are never silently dropped.
    CHECK(tx.push(uwb::protocol::bytesOf(frameOf(3)), TxPriority::High) == PushOutcome::RejectedFull);
    CHECK(tx.droppedCount() == 0);
    CHECK(tx.pending(TxQueueClass::High) == 2);
}

TEST_CASE("normal queue overflow drops the oldest frame and counts it", "[unit][server][queue]") {
    ConnectionTxQueue tx(8, 3);

    for (std::uint8_t i = 1; i <= 5; ++i) {
        (void)tx.push(uwb::protocol::bytesOf(frameOf(i)), TxPriority::Normal);
    }

    CHECK(tx.droppedCount() == 2);
    CHECK(tx.pending(TxQueueClass::Normal) == 3);

    const auto next = tx.popNext();
    REQUIRE(next.has_value());
    CHECK(next->front() == 3); // 1 and 2 were sacrificed
}

TEST_CASE("peekNext does not remove the frame", "[unit][server][queue]") {
    ConnectionTxQueue tx(4, 4);
    (void)tx.push(uwb::protocol::bytesOf(frameOf(7)), TxPriority::Normal);

    const ByteBuffer *peeked = tx.peekNext();
    REQUIRE(peeked != nullptr);
    CHECK(peeked->front() == 7);
    CHECK(tx.pending() == 1);

    tx.popFront();
    CHECK(tx.empty());
    CHECK(tx.peekNext() == nullptr);
}

TEST_CASE("clear drops everything and keeps capacity", "[unit][server][queue]") {
    ConnectionTxQueue tx(2, 2);
    (void)tx.push(uwb::protocol::bytesOf(frameOf(1)), TxPriority::High);
    (void)tx.push(uwb::protocol::bytesOf(frameOf(2)), TxPriority::Normal);
    tx.clear();

    CHECK(tx.empty());
    CHECK(tx.capacity(TxQueueClass::High) == 2);
    CHECK(tx.capacity(TxQueueClass::Normal) == 2);
}
