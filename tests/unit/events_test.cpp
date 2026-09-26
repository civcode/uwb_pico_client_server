#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "uwb/protocol/events.hpp"
#include "uwb/protocol/generic_header.hpp"

#include "gen/golden_vectors.hpp"
#include "test_helpers.hpp"

using namespace uwb::protocol;
using uwb::test::sameBytes;
using uwb::test::view;

TEST_CASE("Event envelope golden (§32)", "[protocol][event][golden]") {
    auto event = decodeEventNotification(view(uwb::test::kEventNotificationPayload));
    REQUIRE(event.ok());
    CHECK(event->eventId == static_cast<std::uint16_t>(EventId::UwbMeasurement));
    CHECK(event->formatVersion == 1);
    CHECK(event->flags == 0);
    CHECK(event->streamId == 1);
    CHECK(event->sequence == 7);
    CHECK(event->timestampUs == 1712345678901234ULL);
    CHECK(event->payload.size() == kMeasurementEventPayloadSize);

    // §32 fixed envelope part is 20 bytes; the rest is the format-specific payload.
    CHECK(uwb::test::kEventNotificationPayload.size() == 20 + kMeasurementEventPayloadSize);
    CHECK(event->payload.size() == uwb::test::kMeasurementEventPayload.size());

    ByteBuffer encoded = encodeEventNotification(event.value());
    CHECK(sameBytes(bytesOf(encoded), view(uwb::test::kEventNotificationPayload)));
}

TEST_CASE("Measurement event golden (§34)", "[protocol][event][golden]") {
    auto measurement = decodeMeasurementEvent(view(uwb::test::kMeasurementEventPayload));
    REQUIRE(measurement.ok());
    CHECK(measurement->networkId == 0x0001);
    CHECK(measurement->anchorId == 0x1001);
    CHECK(measurement->tagId == 0x2001);
    CHECK(measurement->flags == 0x03FF);
    CHECK(measurement->rawRangeMm == 1234);
    CHECK(measurement->correctedRangeMm == 1200);
    CHECK(measurement->rawAzimuthMilliDeg == 45000);
    CHECK(measurement->correctedAzimuthMilliDeg == 44500);
    CHECK(measurement->rawElevationMilliDeg == -1000);
    CHECK(measurement->correctedElevationMilliDeg == -950);
    CHECK(measurement->quality == 850);
    CHECK(measurement->reserved == 0);
    CHECK(sameBytes(bytesOf(encodeMeasurementEvent(measurement.value())),
                          view(uwb::test::kMeasurementEventPayload)));
    CHECK(kMeasurementEventPayloadSize == 36);
}

TEST_CASE("Measurement validity flags (§34)", "[protocol][event]") {
    MeasurementEvent event;
    event.flags = MeasurementFlag::RawRangeValid | MeasurementFlag::CorrectedRangeValid |
                  MeasurementFlag::NetworkIdValid | MeasurementFlag::AnchorIdValid | MeasurementFlag::TagIdValid;
    event.rawRangeMm = 500;
    event.quality = 1000; // 0..1000 range

    ByteBuffer bytes = encodeMeasurementEvent(event);
    auto decoded = decodeMeasurementEvent(bytesOf(bytes));
    REQUIRE(decoded.ok());
    CHECK(hasFlag(decoded->flags, MeasurementFlag::RawRangeValid));
    CHECK(hasFlag(decoded->flags, MeasurementFlag::CorrectedRangeValid));
    CHECK_FALSE(hasFlag(decoded->flags, MeasurementFlag::RawAzimuthValid));

    // Reserved flag bits 10..15 must be zero.
    MeasurementEvent bad = event;
    bad.flags = static_cast<MeasurementFlags>(bad.flags | (1U << 10U));
    auto rejected = decodeMeasurementEvent(bytesOf(encodeMeasurementEvent(bad)));
    REQUIRE(rejected.failed());
    CHECK(rejected.code() == ProtocolErrorCode::InvalidReservedBits);

    MeasurementEvent badQuality = event;
    badQuality.quality = 1001;
    CHECK(decodeMeasurementEvent(bytesOf(encodeMeasurementEvent(badQuality))).failed());

    // Length must be exact.
    CHECK(decodeMeasurementEvent(ConstBytes{bytes.data(), 34}).failed());
}

TEST_CASE("Nested event decode: frame -> envelope -> format payload (§32, §34)", "[protocol][event][golden]") {
    EventNotification event;
    event.eventId = static_cast<std::uint16_t>(EventId::UwbMeasurement);
    event.formatVersion = 1;
    event.flags = 0;
    event.streamId = 1;
    event.reserved = 0;
    event.sequence = 7;
    event.timestampUs = 1712345678901234ULL;
    event.payload = ByteBuffer{uwb::test::kMeasurementEventPayload.begin(),
                               uwb::test::kMeasurementEventPayload.end()};

    auto frameBytes = encodeFrame(PayloadType::EventNotification, bytesOf(encodeEventNotification(event)));
    REQUIRE(frameBytes.ok());
    CHECK(sameBytes(bytesOf(frameBytes.value()), view(uwb::test::kEventNotificationFrame)));

    ConstBytes frame = bytesOf(frameBytes.value());
    auto header = decodeGenericHeader(frame.subspan(0, kGenericHeaderSize));
    REQUIRE(header.ok());
    CHECK(header->payloadType == PayloadType::EventNotification);

    auto envelope = decodeEventNotification(frame.subspan(kGenericHeaderSize));
    REQUIRE(envelope.ok());
    auto measurement = decodeMeasurementEvent(envelope->payloadBytes());
    REQUIRE(measurement.ok());
    CHECK(measurement->correctedRangeMm == 1200);
}

TEST_CASE("Local position event golden (§35)", "[protocol][event][golden]") {
    auto position = decodeLocalPositionEvent(view(uwb::test::kLocalPositionEventPayload));
    REQUIRE(position.ok());
    CHECK(position->networkId == 0x0001);
    CHECK(position->tagId == 0x2001);
    CHECK(position->xMm == 1000);
    CHECK(position->yMm == -2500);
    CHECK(position->zMm == 150);
    CHECK(position->quality == 700);
    CHECK(position->flags == 0x0003);
    CHECK(sameBytes(bytesOf(encodeLocalPositionEvent(position.value())),
                          view(uwb::test::kLocalPositionEventPayload)));
    CHECK(kLocalPositionEventPayloadSize == 20);
}

TEST_CASE("Stream status event golden (§36)", "[protocol][event][golden]") {
    auto status = decodeStreamStatusEvent(view(uwb::test::kStreamStatusEventPayload));
    REQUIRE(status.ok());
    CHECK(status->streamId == 1);
    CHECK(status->state == static_cast<std::uint8_t>(StreamState::Failed));
    CHECK(status->reason == static_cast<std::uint8_t>(StreamStatusReason::RecordingQueueOverflow));
    CHECK(status->droppedCount == 5);
    CHECK(sameBytes(bytesOf(encodeStreamStatusEvent(status.value())), view(uwb::test::kStreamStatusEventPayload)));

    // §36: state and reason are enumerated.
    ByteBuffer badState = encodeStreamStatusEvent(StreamStatusEvent{1, 0x04, 0x00, 0});
    CHECK(decodeStreamStatusEvent(bytesOf(badState)).failed());
    ByteBuffer badReason = encodeStreamStatusEvent(StreamStatusEvent{1, 0x01, 0x05, 0});
    CHECK(decodeStreamStatusEvent(bytesOf(badReason)).failed());
}

TEST_CASE("Event envelope rejects unknown ids and reserved bits (§32, §33)", "[protocol][event]") {
    EventNotification event;
    event.eventId = 0x00F0;
    event.streamId = 2;
    event.payload = encodeStreamStatusEvent(StreamStatusEvent{2, 0x01, 0x00, 0});

    ByteBuffer bytes = encodeEventNotification(event);

    // Reserved field (offset 6) must be zero.
    bytes[6] = 0x01;
    auto rejected = decodeEventNotification(bytesOf(bytes));
    REQUIRE(rejected.failed());
    CHECK(rejected.code() == ProtocolErrorCode::InvalidReservedBits);

    // Unknown Event ID.
    EventNotification unknown = event;
    unknown.eventId = 0x1234;
    CHECK(decodeEventNotification(bytesOf(encodeEventNotification(unknown))).failed());

    // Truncated envelope.
    CHECK(decodeEventNotification(ConstBytes{bytes.data(), 19}).failed());
}
