#pragma once

#include <cstdint>

#include "uwb/protocol/bytes.hpp"
#include "uwb/protocol/result.hpp"

namespace uwb::protocol {

// Specification §33.
enum class EventId : std::uint16_t {
    UwbMeasurement = 0x0001,
    UwbLocalPosition = 0x0002,
    SensorData = 0x0003,
    UwbStatus = 0x0004,
    DiagnosticLog = 0x0005,
    StreamStatus = 0x00F0,
};

[[nodiscard]] constexpr bool isKnownEventId(std::uint16_t value) noexcept {
    switch (value) {
    case 0x0001: case 0x0002: case 0x0003: case 0x0004: case 0x0005: case 0x00F0:
        return true;
    default:
        return false;
    }
}

// Specification §31.2.
enum class StreamMode : std::uint8_t {
    Live = 0x01,
    Recording = 0x02,
};

[[nodiscard]] constexpr bool isKnownStreamMode(std::uint8_t value) noexcept {
    return value == 0x01 || value == 0x02;
}

// Stream state reported by EventControl query and Stream Status events
// (specification §31.6, §36).
enum class StreamState : std::uint8_t {
    Active = 0x01,
    Stopped = 0x02,
    Failed = 0x03,
};

[[nodiscard]] constexpr bool isKnownStreamState(std::uint8_t value) noexcept {
    return value >= 0x01 && value <= 0x03;
}

// Specification §36 reason codes.
enum class StreamStatusReason : std::uint8_t {
    None = 0x00,
    RecordingQueueOverflow = 0x01,
    SourceCapabilityLost = 0x02,
    UwbBackendError = 0x03,
    ConnectionShuttingDown = 0x04,
};

[[nodiscard]] constexpr bool isKnownStreamStatusReason(std::uint8_t value) noexcept {
    return value <= 0x04;
}

inline constexpr std::uint8_t kEventFormatVersion1 = 1;

// ---------------------------------------------------------------------------
// Event Notification envelope (specification §32)
// ---------------------------------------------------------------------------
struct EventNotification {
    std::uint16_t eventId = 0;
    std::uint8_t formatVersion = kEventFormatVersion1;
    std::uint8_t flags = 0;
    std::uint16_t streamId = 0;
    std::uint16_t reserved = 0; // must be zero
    std::uint32_t sequence = 0;
    std::uint64_t timestampUs = 0;
    ByteBuffer payload;

    [[nodiscard]] ConstBytes payloadBytes() const noexcept { return bytesOf(payload); }
};

[[nodiscard]] Result<EventNotification> decodeEventNotification(ConstBytes payload) noexcept;
[[nodiscard]] ByteBuffer encodeEventNotification(const EventNotification &value) noexcept;

// ---------------------------------------------------------------------------
// UWB Measurement event, format v1 (specification §34)
// ---------------------------------------------------------------------------
enum class MeasurementFlag : std::uint16_t {
    RawRangeValid = 1U << 0U,
    CorrectedRangeValid = 1U << 1U,
    RawAzimuthValid = 1U << 2U,
    CorrectedAzimuthValid = 1U << 3U,
    RawElevationValid = 1U << 4U,
    CorrectedElevationValid = 1U << 5U,
    QualityValid = 1U << 6U,
    NetworkIdValid = 1U << 7U,
    AnchorIdValid = 1U << 8U,
    TagIdValid = 1U << 9U,
};

using MeasurementFlags = std::uint16_t;

[[nodiscard]] constexpr MeasurementFlags operator|(MeasurementFlag a, MeasurementFlag b) noexcept {
    return static_cast<MeasurementFlags>(a) | static_cast<MeasurementFlags>(b);
}

[[nodiscard]] constexpr MeasurementFlags operator|(MeasurementFlags a, MeasurementFlag b) noexcept {
    return static_cast<MeasurementFlags>(a | static_cast<MeasurementFlags>(b));
}

[[nodiscard]] constexpr bool hasFlag(MeasurementFlags flags, MeasurementFlag flag) noexcept {
    return (flags & static_cast<MeasurementFlags>(flag)) != 0;
}

inline constexpr MeasurementFlags kMeasurementKnownFlags =
    static_cast<MeasurementFlags>(MeasurementFlag::RawRangeValid) |
    static_cast<MeasurementFlags>(MeasurementFlag::CorrectedRangeValid) |
    static_cast<MeasurementFlags>(MeasurementFlag::RawAzimuthValid) |
    static_cast<MeasurementFlags>(MeasurementFlag::CorrectedAzimuthValid) |
    static_cast<MeasurementFlags>(MeasurementFlag::RawElevationValid) |
    static_cast<MeasurementFlags>(MeasurementFlag::CorrectedElevationValid) |
    static_cast<MeasurementFlags>(MeasurementFlag::QualityValid) |
    static_cast<MeasurementFlags>(MeasurementFlag::NetworkIdValid) |
    static_cast<MeasurementFlags>(MeasurementFlag::AnchorIdValid) |
    static_cast<MeasurementFlags>(MeasurementFlag::TagIdValid);

inline constexpr std::uint16_t kMeasurementEventPayloadSize = 36;

struct MeasurementEvent {
    std::uint16_t networkId = 0;
    std::uint16_t anchorId = 0; // source node
    std::uint16_t tagId = 0;    // target node
    MeasurementFlags flags = 0;
    std::int32_t rawRangeMm = 0;
    std::int32_t correctedRangeMm = 0;
    std::int32_t rawAzimuthMilliDeg = 0;
    std::int32_t correctedAzimuthMilliDeg = 0;
    std::int32_t rawElevationMilliDeg = 0;
    std::int32_t correctedElevationMilliDeg = 0;
    std::uint16_t quality = 0; // 0..1000 where possible
    std::uint16_t reserved = 0;
};

[[nodiscard]] Result<MeasurementEvent> decodeMeasurementEvent(ConstBytes payload) noexcept;
[[nodiscard]] ByteBuffer encodeMeasurementEvent(const MeasurementEvent &value) noexcept;

// ---------------------------------------------------------------------------
// UWB Local Position event, format v1 (specification §35)
// ---------------------------------------------------------------------------
inline constexpr std::uint16_t kLocalPositionEventPayloadSize = 20;

struct LocalPositionEvent {
    std::uint16_t networkId = 0;
    std::uint16_t tagId = 0;
    std::int32_t xMm = 0;
    std::int32_t yMm = 0;
    std::int32_t zMm = 0;
    std::uint16_t quality = 0;
    std::uint16_t flags = 0;
};

[[nodiscard]] Result<LocalPositionEvent> decodeLocalPositionEvent(ConstBytes payload) noexcept;
[[nodiscard]] ByteBuffer encodeLocalPositionEvent(const LocalPositionEvent &value) noexcept;

// ---------------------------------------------------------------------------
// Stream Status event, format v1 (specification §36)
// ---------------------------------------------------------------------------
inline constexpr std::uint16_t kStreamStatusEventPayloadSize = 8;

struct StreamStatusEvent {
    std::uint16_t streamId = 0;
    std::uint8_t state = static_cast<std::uint8_t>(StreamState::Active);
    std::uint8_t reason = static_cast<std::uint8_t>(StreamStatusReason::None);
    std::uint32_t droppedCount = 0;
};

[[nodiscard]] Result<StreamStatusEvent> decodeStreamStatusEvent(ConstBytes payload) noexcept;
[[nodiscard]] ByteBuffer encodeStreamStatusEvent(const StreamStatusEvent &value) noexcept;

} // namespace uwb::protocol
