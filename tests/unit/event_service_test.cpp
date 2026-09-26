#include <catch2/catch_all.hpp>

#include "uwb/protocol/events.hpp"
#include "uwb/protocol/generic_header.hpp"
#include "uwb/server/event_service.hpp"

#include "server_fakes.hpp"
#include "test_helpers.hpp"

using namespace uwb::server;
using namespace uwb::test;
using uwb::protocol::ByteBuffer;
using uwb::protocol::EventId;
using uwb::protocol::EventNotification;
using uwb::protocol::StreamMode;
using uwb::protocol::StreamState;
using uwb::protocol::StreamStatusReason;
using uwb::protocol::decodeEventNotification;
using uwb::protocol::decodeGenericHeader;

namespace {

constexpr ConnectionId kConn = 7;

ByteBuffer sample(std::uint8_t marker) {
    return ByteBuffer{marker, marker, marker};
}

struct Fixture {
    ServerConfig config{};
    FakeClock clock{1'000'000};
    EventService service{config, clock};
    ConnectionTxQueue tx{8, 8};
};

std::vector<EventNotification> drain(EventService &service, ConnectionTxQueue &tx, FakeClock &clock,
                                     ConnectionId connection) {
    (void)service.flush(connection, tx, clock.monotonicUs());
    std::vector<EventNotification> out;
    while (auto frameBytes = tx.popNext()) {
        auto header = decodeGenericHeader(uwb::protocol::ConstBytes{frameBytes->data(), frameBytes->size()});
        REQUIRE(header.ok());
        REQUIRE(header.value().payloadType == uwb::protocol::PayloadType::EventNotification);
        ConstBytes payload{frameBytes->data() + uwb::protocol::kGenericHeaderSize,
                          frameBytes->size() - uwb::protocol::kGenericHeaderSize};
        auto note = decodeEventNotification(payload);
        REQUIRE(note.ok());
        out.push_back(note.value());
    }
    return out;
}

} // namespace

TEST_CASE("subscribe assigns a stream id and reports the queue capacity", "[unit][server][events]") {
    Fixture f;
    auto stream = f.service.subscribe(kConn, EventId::UwbMeasurement, StreamMode::Live, 0);

    REQUIRE(stream.ok());
    CHECK(stream.value.streamId != 0);
    CHECK(stream.value.eventId == EventId::UwbMeasurement);
    CHECK(stream.value.mode == StreamMode::Live);
    CHECK(stream.value.state == StreamState::Active);
    CHECK(f.service.queueCapacity(StreamMode::Live) == uwb::protocol::kLiveStreamQueueCapacity);
    CHECK(f.service.streamCount(kConn) == 1);
}

TEST_CASE("requested rate is clamped to the configured maximum events per second",
          "[unit][server][events][rate-limit]") {
    Fixture f;
    f.config.maxEventsPerSecond = 50; // 20 ms minimum period

    auto stream = f.service.subscribe(kConn, EventId::UwbMeasurement, StreamMode::Live, 1);
    REQUIRE(stream.ok());
    CHECK(stream.value.periodMs == 20);

    // Samples arriving faster than the agreed period are skipped, not queued.
    (void)f.service.publish(EventId::UwbMeasurement, uwb::protocol::bytesOf(sample(1)));
    f.clock.advanceMs(5);
    (void)f.service.publish(EventId::UwbMeasurement, uwb::protocol::bytesOf(sample(2)));
    f.clock.advanceMs(30);
    (void)f.service.publish(EventId::UwbMeasurement, uwb::protocol::bytesOf(sample(3)));

    const auto notes = drain(f.service, f.tx, f.clock, kConn);
    CHECK(notes.size() == 2);
}

TEST_CASE("too many streams on one connection is refused", "[unit][server][events]") {
    Fixture f;
    f.config.maxStreamsPerConnection = 2;

    CHECK(f.service.subscribe(kConn, EventId::UwbMeasurement, StreamMode::Live, 0).ok());
    CHECK(f.service.subscribe(kConn, EventId::UwbLocalPosition, StreamMode::Live, 0).ok());
    const auto third = f.service.subscribe(kConn, EventId::UwbStatus, StreamMode::Live, 0);
    CHECK(third.status == ServerStatus::TooManyStreams);
    CHECK(nrcFor(third.status) == uwb::protocol::ServiceNrc::ConditionsNotCorrect);
}

TEST_CASE("live streams keep streaming and count dropped samples", "[unit][server][events][live]") {
    Fixture f;
    auto stream = f.service.subscribe(kConn, EventId::UwbMeasurement, StreamMode::Live, 0);
    REQUIRE(stream.ok());

    const std::uint32_t capacity = f.service.queueCapacity(StreamMode::Live);
    for (std::uint32_t i = 0; i < capacity + 3; ++i) {
        (void)f.service.publish(EventId::UwbMeasurement, uwb::protocol::bytesOf(sample(static_cast<std::uint8_t>(i))));
    }

    const auto info = f.service.query(kConn, stream.value.streamId);
    REQUIRE(info.ok());
    CHECK(info.value.state == StreamState::Active); // never fails, just drops
    CHECK(info.value.droppedCount == 3);
    CHECK(f.service.totalDropped(kConn) == 3);

    const auto notes = drain(f.service, f.tx, f.clock, kConn);
    CHECK(notes.size() == capacity);
    // The three newest samples survive (§37 drop-oldest).
    CHECK(notes.front().payload.front() == capacity - 1);
}

TEST_CASE("recording streams fail visibly on overflow instead of dropping",
          "[unit][server][events][recording]") {
    Fixture f;
    f.config.queues.recordingStreamQueue = 2;

    auto stream = f.service.subscribe(kConn, EventId::UwbMeasurement, StreamMode::Recording, 0);
    REQUIRE(stream.ok());

    (void)f.service.publish(EventId::UwbMeasurement, uwb::protocol::bytesOf(sample(1)));
    (void)f.service.publish(EventId::UwbMeasurement, uwb::protocol::bytesOf(sample(2)));
    (void)f.service.publish(EventId::UwbMeasurement, uwb::protocol::bytesOf(sample(3)));

    const auto info = f.service.query(kConn, stream.value.streamId);
    REQUIRE(info.ok());
    CHECK(info.value.state == StreamState::Failed);

    const auto notes = drain(f.service, f.tx, f.clock, kConn);
    // The client is told about the failure with a StreamStatus event (§36).
    REQUIRE_FALSE(notes.empty());
    const EventNotification *status = nullptr;
    for (const EventNotification &note : notes) {
        if (note.eventId == static_cast<std::uint16_t>(EventId::StreamStatus)) {
            status = &note;
        }
    }
    REQUIRE(status != nullptr);
    auto decoded = uwb::protocol::decodeStreamStatusEvent(uwb::protocol::ConstBytes{status->payload.data(),
                                                                                    status->payload.size()});
    REQUIRE(decoded.ok());
    CHECK(decoded.value().state == static_cast<std::uint8_t>(StreamState::Failed));
    CHECK(decoded.value().reason == static_cast<std::uint8_t>(StreamStatusReason::RecordingQueueOverflow));
}

TEST_CASE("event notification sequence is per stream and starts at 1", "[unit][server][events]") {
    Fixture f;
    auto a = f.service.subscribe(kConn, EventId::UwbMeasurement, StreamMode::Live, 0);
    auto b = f.service.subscribe(kConn, EventId::UwbLocalPosition, StreamMode::Live, 0);
    REQUIRE(a.ok());
    REQUIRE(b.ok());

    (void)f.service.publish(EventId::UwbMeasurement, uwb::protocol::bytesOf(sample(1)));
    (void)f.service.publish(EventId::UwbLocalPosition, uwb::protocol::bytesOf(sample(2)));
    (void)f.service.publish(EventId::UwbMeasurement, uwb::protocol::bytesOf(sample(3)));

    const auto notes = drain(f.service, f.tx, f.clock, kConn);
    REQUIRE(notes.size() == 3);

    std::uint32_t measurementSequence = 0;
    std::uint32_t positionSequence = 0;
    for (const EventNotification &note : notes) {
        if (note.eventId == static_cast<std::uint16_t>(EventId::UwbMeasurement)) {
            CHECK(++measurementSequence == note.sequence);
            CHECK(note.streamId == a.value.streamId);
        } else {
            CHECK(++positionSequence == note.sequence);
            CHECK(note.streamId == b.value.streamId);
        }
    }
    CHECK(measurementSequence == 2);
    CHECK(positionSequence == 1);
}

TEST_CASE("unsubscribe and unsubscribeAll clean up streams", "[unit][server][events]") {
    Fixture f;
    auto a = f.service.subscribe(kConn, EventId::UwbMeasurement, StreamMode::Live, 0);
    auto b = f.service.subscribe(kConn, EventId::UwbStatus, StreamMode::Live, 0);
    REQUIRE(a.ok());
    REQUIRE(b.ok());

    const auto removed = f.service.unsubscribe(kConn, a.value.streamId);
    REQUIRE(removed.ok());
    CHECK(removed.value.state == StreamState::Stopped);
    CHECK(f.service.streamCount(kConn) == 1);

    const auto unknown = f.service.unsubscribe(kConn, 999);
    CHECK(unknown.status == ServerStatus::UnknownStream);

    CHECK(f.service.unsubscribeAll(kConn) == 1);
    CHECK(f.service.streamCount(kConn) == 0);
    CHECK(f.service.unsubscribeAll(kConn) == 0);
}

TEST_CASE("events are delivered only to matching subscriptions", "[unit][server][events]") {
    Fixture f;
    (void)f.service.subscribe(kConn, EventId::UwbMeasurement, StreamMode::Live, 0);
    (void)f.service.subscribe(11, EventId::UwbLocalPosition, StreamMode::Live, 0);

    CHECK(f.service.publish(EventId::UwbMeasurement, uwb::protocol::bytesOf(sample(1))) == 1);
    CHECK(f.service.publish(EventId::DiagnosticLog, uwb::protocol::bytesOf(sample(2))) == 0);

    ConnectionTxQueue other{8, 8};
    CHECK(f.service.flush(11, other, f.clock.monotonicUs()) == 0);
}

TEST_CASE("connectionClosed drops streams and pending samples", "[unit][server][events]") {
    Fixture f;
    (void)f.service.subscribe(kConn, EventId::UwbMeasurement, StreamMode::Live, 0);
    (void)f.service.publish(EventId::UwbMeasurement, uwb::protocol::bytesOf(sample(1)));
    f.service.connectionClosed(kConn);

    CHECK(f.service.streamCount(kConn) == 0);
    CHECK(f.service.flush(kConn, f.tx, f.clock.monotonicUs()) == 0);
}
