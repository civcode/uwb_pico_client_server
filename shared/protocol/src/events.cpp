#include "uwb/protocol/events.hpp"

#include "uwb/protocol/wire_reader.hpp"
#include "uwb/protocol/wire_writer.hpp"

namespace uwb::protocol {

namespace {

constexpr std::size_t kEventEnvelopeFixedSize = 2 + 1 + 1 + 2 + 2 + 4 + 8;

} // namespace

// ---------------------------------------------------------------------------
// Event Notification envelope
// ---------------------------------------------------------------------------
Result<EventNotification> decodeEventNotification(ConstBytes payload) noexcept {
    if (payload.size() < kEventEnvelopeFixedSize) {
        return Result<EventNotification>::error(ProtocolErrorCode::TruncatedPayload,
                                                static_cast<std::uint32_t>(payload.size()));
    }

    WireReader reader{payload};
    EventNotification event;

    if (!reader.readU16(event.eventId)) {
        return Result<EventNotification>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (!reader.readU8(event.formatVersion) || !reader.readU8(event.flags)) {
        return Result<EventNotification>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (!reader.readU16(event.streamId)) {
        return Result<EventNotification>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (!reader.readU16(event.reserved)) {
        return Result<EventNotification>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (event.reserved != 0) {
        return Result<EventNotification>::error(ProtocolErrorCode::InvalidReservedBits, event.reserved);
    }
    if (!reader.readU32(event.sequence)) {
        return Result<EventNotification>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (!reader.readU64(event.timestampUs)) {
        return Result<EventNotification>::error(ProtocolErrorCode::TruncatedPayload);
    }

    if (!isKnownEventId(event.eventId)) {
        return Result<EventNotification>::error(ProtocolErrorCode::InvalidField, event.eventId);
    }

    event.payload.assign(reader.remainingBytes().begin(), reader.remainingBytes().end());
    return Result<EventNotification>::ok(std::move(event));
}

ByteBuffer encodeEventNotification(const EventNotification &value) noexcept {
    ByteBuffer out;
    out.reserve(kEventEnvelopeFixedSize + value.payload.size());
    WireWriter w{out};
    w.writeU16(value.eventId);
    w.writeU8(value.formatVersion);
    w.writeU8(value.flags);
    w.writeU16(value.streamId);
    w.writeU16(value.reserved);
    w.writeU32(value.sequence);
    w.writeU64(value.timestampUs);
    w.writeBytes(bytesOf(value.payload));
    return out;
}

// ---------------------------------------------------------------------------
// UWB Measurement event v1
// ---------------------------------------------------------------------------
Result<MeasurementEvent> decodeMeasurementEvent(ConstBytes payload) noexcept {
    if (payload.size() != kMeasurementEventPayloadSize) {
        return Result<MeasurementEvent>::error(ProtocolErrorCode::UnexpectedPayloadLength,
                                               static_cast<std::uint32_t>(payload.size()));
    }

    WireReader reader{payload};
    MeasurementEvent event;

    if (!reader.readU16(event.networkId) || !reader.readU16(event.anchorId) || !reader.readU16(event.tagId) ||
        !reader.readU16(event.flags)) {
        return Result<MeasurementEvent>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (!reader.readI32(event.rawRangeMm) || !reader.readI32(event.correctedRangeMm)) {
        return Result<MeasurementEvent>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (!reader.readI32(event.rawAzimuthMilliDeg) || !reader.readI32(event.correctedAzimuthMilliDeg)) {
        return Result<MeasurementEvent>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (!reader.readI32(event.rawElevationMilliDeg) || !reader.readI32(event.correctedElevationMilliDeg)) {
        return Result<MeasurementEvent>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (!reader.readU16(event.quality) || !reader.readU16(event.reserved)) {
        return Result<MeasurementEvent>::error(ProtocolErrorCode::TruncatedPayload);
    }

    if ((event.flags & ~kMeasurementKnownFlags) != 0) {
        return Result<MeasurementEvent>::error(ProtocolErrorCode::InvalidReservedBits, event.flags);
    }
    if (event.quality > 1000) {
        return Result<MeasurementEvent>::error(ProtocolErrorCode::InvalidField, event.quality);
    }

    return Result<MeasurementEvent>::ok(event);
}

ByteBuffer encodeMeasurementEvent(const MeasurementEvent &value) noexcept {
    ByteBuffer out;
    out.reserve(kMeasurementEventPayloadSize);
    WireWriter w{out};
    w.writeU16(value.networkId);
    w.writeU16(value.anchorId);
    w.writeU16(value.tagId);
    w.writeU16(value.flags);
    w.writeI32(value.rawRangeMm);
    w.writeI32(value.correctedRangeMm);
    w.writeI32(value.rawAzimuthMilliDeg);
    w.writeI32(value.correctedAzimuthMilliDeg);
    w.writeI32(value.rawElevationMilliDeg);
    w.writeI32(value.correctedElevationMilliDeg);
    w.writeU16(value.quality);
    w.writeU16(value.reserved);
    return out;
}

// ---------------------------------------------------------------------------
// UWB Local Position event v1
// ---------------------------------------------------------------------------
Result<LocalPositionEvent> decodeLocalPositionEvent(ConstBytes payload) noexcept {
    if (payload.size() != kLocalPositionEventPayloadSize) {
        return Result<LocalPositionEvent>::error(ProtocolErrorCode::UnexpectedPayloadLength,
                                                 static_cast<std::uint32_t>(payload.size()));
    }

    WireReader reader{payload};
    LocalPositionEvent event;

    if (!reader.readU16(event.networkId) || !reader.readU16(event.tagId)) {
        return Result<LocalPositionEvent>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (!reader.readI32(event.xMm) || !reader.readI32(event.yMm) || !reader.readI32(event.zMm)) {
        return Result<LocalPositionEvent>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (!reader.readU16(event.quality) || !reader.readU16(event.flags)) {
        return Result<LocalPositionEvent>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (event.quality > 1000) {
        return Result<LocalPositionEvent>::error(ProtocolErrorCode::InvalidField, event.quality);
    }

    return Result<LocalPositionEvent>::ok(event);
}

ByteBuffer encodeLocalPositionEvent(const LocalPositionEvent &value) noexcept {
    ByteBuffer out;
    out.reserve(kLocalPositionEventPayloadSize);
    WireWriter w{out};
    w.writeU16(value.networkId);
    w.writeU16(value.tagId);
    w.writeI32(value.xMm);
    w.writeI32(value.yMm);
    w.writeI32(value.zMm);
    w.writeU16(value.quality);
    w.writeU16(value.flags);
    return out;
}

// ---------------------------------------------------------------------------
// Stream Status event v1
// ---------------------------------------------------------------------------
Result<StreamStatusEvent> decodeStreamStatusEvent(ConstBytes payload) noexcept {
    if (payload.size() != kStreamStatusEventPayloadSize) {
        return Result<StreamStatusEvent>::error(ProtocolErrorCode::UnexpectedPayloadLength,
                                                static_cast<std::uint32_t>(payload.size()));
    }

    WireReader reader{payload};
    StreamStatusEvent event;

    if (!reader.readU16(event.streamId)) {
        return Result<StreamStatusEvent>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (!reader.readU8(event.state) || !isKnownStreamState(event.state)) {
        return Result<StreamStatusEvent>::error(ProtocolErrorCode::InvalidField, event.state);
    }
    if (!reader.readU8(event.reason) || !isKnownStreamStatusReason(event.reason)) {
        return Result<StreamStatusEvent>::error(ProtocolErrorCode::InvalidField, event.reason);
    }
    if (!reader.readU32(event.droppedCount)) {
        return Result<StreamStatusEvent>::error(ProtocolErrorCode::TruncatedPayload);
    }

    return Result<StreamStatusEvent>::ok(event);
}

ByteBuffer encodeStreamStatusEvent(const StreamStatusEvent &value) noexcept {
    ByteBuffer out;
    out.reserve(kStreamStatusEventPayloadSize);
    WireWriter w{out};
    w.writeU16(value.streamId);
    w.writeU8(value.state);
    w.writeU8(value.reason);
    w.writeU32(value.droppedCount);
    return out;
}

} // namespace uwb::protocol
