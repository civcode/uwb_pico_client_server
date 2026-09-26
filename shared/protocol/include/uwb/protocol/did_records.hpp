#pragma once

#include <cstdint>
#include <optional>

#include "uwb/protocol/bytes.hpp"
#include "uwb/protocol/capabilities.hpp"
#include "uwb/protocol/dids.hpp"
#include "uwb/protocol/result.hpp"
#include "uwb/protocol/uuid.hpp"

namespace uwb::protocol {

// Typed record codecs for every DID in the specification §40 layout tables.
// These are the only place where DID record layouts are defined; the DID
// framework in server-core and the client DID cache use them exclusively.

// ---------------------------------------------------------------------------
// Fixed-size helpers
// ---------------------------------------------------------------------------
[[nodiscard]] ByteBuffer encodeU8Record(std::uint8_t value) noexcept;
[[nodiscard]] ByteBuffer encodeU16Record(std::uint16_t value) noexcept;
[[nodiscard]] ByteBuffer encodeU32Record(std::uint32_t value) noexcept;
[[nodiscard]] ByteBuffer encodeU64Record(std::uint64_t value) noexcept;
[[nodiscard]] ByteBuffer encodeI16Record(std::int16_t value) noexcept;
[[nodiscard]] ByteBuffer encodeI32Record(std::int32_t value) noexcept;

[[nodiscard]] Result<std::uint8_t> decodeU8Record(ConstBytes data) noexcept;
[[nodiscard]] Result<std::uint16_t> decodeU16Record(ConstBytes data) noexcept;
[[nodiscard]] Result<std::uint32_t> decodeU32Record(ConstBytes data) noexcept;
[[nodiscard]] Result<std::uint64_t> decodeU64Record(ConstBytes data) noexcept;
[[nodiscard]] Result<std::int16_t> decodeI16Record(ConstBytes data) noexcept;
[[nodiscard]] Result<std::int32_t> decodeI32Record(ConstBytes data) noexcept;

// Length-prefixed text records (specification §40.3, §40.6).
[[nodiscard]] ByteBuffer encodeTextRecord8(const ByteBuffer &text) noexcept;
[[nodiscard]] Result<ByteBuffer> decodeTextRecord8(ConstBytes data, std::size_t maxTextLength) noexcept;

// ---------------------------------------------------------------------------
// 0xF000 / 0xF001
// ---------------------------------------------------------------------------
[[nodiscard]] ByteBuffer encodeUuidRecord(const Uuid &uuid) noexcept;
[[nodiscard]] Result<Uuid> decodeUuidRecord(ConstBytes data) noexcept;

[[nodiscard]] ByteBuffer encodeBoardIdRecord(const std::array<std::uint8_t, 8> &boardId) noexcept;
[[nodiscard]] Result<std::array<std::uint8_t, 8>> decodeBoardIdRecord(ConstBytes data) noexcept;

// ---------------------------------------------------------------------------
// 0xF004 Pico server firmware version
// ---------------------------------------------------------------------------
struct ServerFirmwareVersionRecord {
    std::uint8_t major = 0;
    std::uint8_t minor = 0;
    std::uint16_t patch = 0;
    std::uint32_t build = 0;
};

[[nodiscard]] ByteBuffer encodeRecord(const ServerFirmwareVersionRecord &value) noexcept;
[[nodiscard]] Result<ServerFirmwareVersionRecord> decodeServerFirmwareVersionRecord(ConstBytes data) noexcept;

// ---------------------------------------------------------------------------
// 0xF006 / 0xF007 / 0xF008 capabilities
// ---------------------------------------------------------------------------
struct CapabilityDetectionRecord {
    CapabilityMask knownMask = 0;
    CapabilityMask detectedSupportedMask = 0;
};

struct CapabilityOverrideRecord {
    CapabilityMask forceOnMask = 0;
    CapabilityMask forceOffMask = 0;
};

[[nodiscard]] ByteBuffer encodeRecord(const CapabilityDetectionRecord &value) noexcept;
[[nodiscard]] Result<CapabilityDetectionRecord> decodeCapabilityDetectionRecord(ConstBytes data) noexcept;

[[nodiscard]] ByteBuffer encodeRecord(const CapabilityOverrideRecord &value) noexcept;
[[nodiscard]] Result<CapabilityOverrideRecord> decodeCapabilityOverrideRecord(ConstBytes data) noexcept;

// ---------------------------------------------------------------------------
// 0xF009 Timeout configuration (bounds: specification §52)
// ---------------------------------------------------------------------------
struct TimeoutConfigRecord {
    std::uint32_t uartCommandTimeoutMs = 0;
    std::uint32_t uartInterCommandGapMs = 0;
    std::uint32_t sessionInactivityTimeoutMs = 0;
    std::uint32_t tcpIdleTimeoutMs = 0;
    std::uint32_t aliveCheckIntervalMs = 0;
};

[[nodiscard]] ByteBuffer encodeRecord(const TimeoutConfigRecord &value) noexcept;
[[nodiscard]] Result<TimeoutConfigRecord> decodeTimeoutConfigRecord(ConstBytes data) noexcept;
[[nodiscard]] bool timeoutConfigWithinBounds(const TimeoutConfigRecord &value) noexcept;

// ---------------------------------------------------------------------------
// 0xF00A Connection status
// ---------------------------------------------------------------------------
struct ConnectionStatusRecord {
    std::uint8_t controlOccupied = 0;
    std::uint8_t activeObservers = 0;
    std::uint8_t maxObservers = 0;
    std::uint8_t activeSession = 0;
    std::uint16_t controlClientLogicalAddress = 0;
    std::uint16_t reserved = 0;
};

[[nodiscard]] ByteBuffer encodeRecord(const ConnectionStatusRecord &value) noexcept;
[[nodiscard]] Result<ConnectionStatusRecord> decodeConnectionStatusRecord(ConstBytes data) noexcept;

// ---------------------------------------------------------------------------
// 0xF010 UWB device parameters
// ---------------------------------------------------------------------------
struct UwbDeviceParametersRecord {
    std::uint16_t id = 0;
    std::uint8_t role = 0;
    std::uint8_t channel = 0;
    std::uint16_t rate = 0;
    std::uint16_t reserved = 0;
};

[[nodiscard]] ByteBuffer encodeRecord(const UwbDeviceParametersRecord &value) noexcept;
[[nodiscard]] Result<UwbDeviceParametersRecord> decodeUwbDeviceParametersRecord(ConstBytes data) noexcept;

// ---------------------------------------------------------------------------
// 0xF011 UWB TWR parameters
// ---------------------------------------------------------------------------
inline constexpr std::uint16_t kTwrFlagKalmanEnabled = 1U << 0U;
inline constexpr std::uint16_t kTwrFlagModulePositioning = 1U << 1U;
inline constexpr std::uint16_t kTwrKnownFlags = kTwrFlagKalmanEnabled | kTwrFlagModulePositioning;

struct UwbTwrParametersRecord {
    std::uint16_t tagCapacity = 0;
    std::uint16_t antennaDelay = 0;
    std::uint16_t flags = 0;
    std::uint8_t positioningDimension = 0;
    std::uint8_t reserved = 0;
    float kalmanQ = 0.0F;
    float kalmanR = 0.0F;
    float correctionParameterA = 0.0F;
    float correctionParameterB = 0.0F;
};

[[nodiscard]] ByteBuffer encodeRecord(const UwbTwrParametersRecord &value) noexcept;
[[nodiscard]] Result<UwbTwrParametersRecord> decodeUwbTwrParametersRecord(ConstBytes data) noexcept;

// ---------------------------------------------------------------------------
// 0xF012 UWB PDoA parameters
// ---------------------------------------------------------------------------
struct UwbPdoaParametersRecord {
    std::uint16_t dlist = 0;
    std::uint16_t klist = 0;
    std::uint32_t network = 0;
    std::uint16_t anchorId = 0;
    std::uint16_t rate = 0;
    std::uint8_t filterEnabled = 0;
    std::uint8_t reserved = 0;
    std::uint16_t userCommand = 0;
    std::int32_t pdoaOffsetRaw = 0;
    std::int32_t rangeOffsetMm = 0;
};

[[nodiscard]] ByteBuffer encodeRecord(const UwbPdoaParametersRecord &value) noexcept;
[[nodiscard]] Result<UwbPdoaParametersRecord> decodeUwbPdoaParametersRecord(ConstBytes data) noexcept;

// ---------------------------------------------------------------------------
// 0xF015 Latest sensor data, 0xF016 latest distance
// ---------------------------------------------------------------------------
struct LatestSensorDataRecord {
    float accX = 0.0F;
    float accY = 0.0F;
    float accZ = 0.0F;
    float angle = 0.0F;
};

[[nodiscard]] ByteBuffer encodeRecord(const LatestSensorDataRecord &value) noexcept;
[[nodiscard]] Result<LatestSensorDataRecord> decodeLatestSensorDataRecord(ConstBytes data) noexcept;

// ---------------------------------------------------------------------------
// 0xF020 Device calibration
// ---------------------------------------------------------------------------
struct DeviceCalibrationRecord {
    std::uint16_t flags = 0;
    std::int32_t rangeScalePpm = 0;
    std::int32_t rangeOffsetMm = 0;
    std::int32_t azimuthOffsetMilliDeg = 0;
    std::int32_t elevationOffsetMilliDeg = 0;
};

[[nodiscard]] ByteBuffer encodeRecord(const DeviceCalibrationRecord &value) noexcept;
[[nodiscard]] Result<DeviceCalibrationRecord> decodeDeviceCalibrationRecord(ConstBytes data) noexcept;

// ---------------------------------------------------------------------------
// Expected record size for fixed-size DIDs; std::nullopt for variable-length
// records (0xF002 name, 0xF005 module version, 0xF017 metadata, 0xF01F complete
// configuration).
// ---------------------------------------------------------------------------
[[nodiscard]] std::optional<std::size_t> expectedDidRecordSize(Did did) noexcept;

} // namespace uwb::protocol
