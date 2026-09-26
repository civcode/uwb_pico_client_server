#include "uwb/protocol/did_records.hpp"

#include <array>
#include <utility>

#include "uwb/protocol/constants.hpp"
#include "uwb/protocol/wire_reader.hpp"
#include "uwb/protocol/wire_writer.hpp"

namespace uwb::protocol {

namespace {

constexpr std::size_t kBoardIdLength = 8;

template <typename T>
[[nodiscard]] Result<T> truncated(std::uint32_t actual) noexcept {
    return Result<T>::error(ProtocolErrorCode::TruncatedPayload, actual);
}

template <typename T>
[[nodiscard]] Result<T> wrongSize(std::uint32_t actual) noexcept {
    return Result<T>::error(ProtocolErrorCode::UnexpectedPayloadLength, actual);
}

} // namespace

// ---------------------------------------------------------------------------
// Scalar records
// ---------------------------------------------------------------------------
ByteBuffer encodeU8Record(std::uint8_t value) noexcept { return ByteBuffer{value}; }

ByteBuffer encodeU16Record(std::uint16_t value) noexcept {
    ByteBuffer out;
    WireWriter w{out};
    w.writeU16(value);
    return out;
}

ByteBuffer encodeU32Record(std::uint32_t value) noexcept {
    ByteBuffer out;
    WireWriter w{out};
    w.writeU32(value);
    return out;
}

ByteBuffer encodeU64Record(std::uint64_t value) noexcept {
    ByteBuffer out;
    WireWriter w{out};
    w.writeU64(value);
    return out;
}

ByteBuffer encodeI16Record(std::int16_t value) noexcept { return encodeU16Record(static_cast<std::uint16_t>(value)); }

ByteBuffer encodeI32Record(std::int32_t value) noexcept { return encodeU32Record(static_cast<std::uint32_t>(value)); }

Result<std::uint8_t> decodeU8Record(ConstBytes data) noexcept {
    if (data.size() != 1) {
        return wrongSize<std::uint8_t>(static_cast<std::uint32_t>(data.size()));
    }
    return Result<std::uint8_t>::ok(data[0]);
}

Result<std::uint16_t> decodeU16Record(ConstBytes data) noexcept {
    if (data.size() != 2) {
        return wrongSize<std::uint16_t>(static_cast<std::uint32_t>(data.size()));
    }
    WireReader reader{data};
    std::uint16_t value = 0;
    if (!reader.readU16(value)) {
        return truncated<std::uint16_t>(static_cast<std::uint32_t>(data.size()));
    }
    return Result<std::uint16_t>::ok(value);
}

Result<std::uint32_t> decodeU32Record(ConstBytes data) noexcept {
    if (data.size() != 4) {
        return wrongSize<std::uint32_t>(static_cast<std::uint32_t>(data.size()));
    }
    WireReader reader{data};
    std::uint32_t value = 0;
    if (!reader.readU32(value)) {
        return truncated<std::uint32_t>(static_cast<std::uint32_t>(data.size()));
    }
    return Result<std::uint32_t>::ok(value);
}

Result<std::uint64_t> decodeU64Record(ConstBytes data) noexcept {
    if (data.size() != 8) {
        return wrongSize<std::uint64_t>(static_cast<std::uint32_t>(data.size()));
    }
    WireReader reader{data};
    std::uint64_t value = 0;
    if (!reader.readU64(value)) {
        return truncated<std::uint64_t>(static_cast<std::uint32_t>(data.size()));
    }
    return Result<std::uint64_t>::ok(value);
}

Result<std::int16_t> decodeI16Record(ConstBytes data) noexcept {
    auto raw = decodeU16Record(data);
    if (!raw) {
        return Result<std::int16_t>::error(raw.error());
    }
    return Result<std::int16_t>::ok(static_cast<std::int16_t>(raw.value()));
}

Result<std::int32_t> decodeI32Record(ConstBytes data) noexcept {
    auto raw = decodeU32Record(data);
    if (!raw) {
        return Result<std::int32_t>::error(raw.error());
    }
    return Result<std::int32_t>::ok(static_cast<std::int32_t>(raw.value()));
}

// ---------------------------------------------------------------------------
// Text records (0xF002 device name, 0xF005 module version)
// ---------------------------------------------------------------------------
ByteBuffer encodeTextRecord8(const ByteBuffer &text) noexcept {
    ByteBuffer out;
    out.reserve(1 + text.size());
    out.push_back(static_cast<std::uint8_t>(text.size()));
    out.insert(out.end(), text.begin(), text.end());
    return out;
}

Result<ByteBuffer> decodeTextRecord8(ConstBytes data, std::size_t maxTextLength) noexcept {
    if (data.empty()) {
        return Result<ByteBuffer>::error(ProtocolErrorCode::TruncatedPayload, 0);
    }
    const std::size_t length = data[0];
    if (length > maxTextLength) {
        return Result<ByteBuffer>::error(ProtocolErrorCode::InvalidField, static_cast<std::uint32_t>(length));
    }
    if (data.size() != 1 + length) {
        return Result<ByteBuffer>::error(ProtocolErrorCode::UnexpectedPayloadLength,
                                         static_cast<std::uint32_t>(data.size()));
    }
    ByteBuffer text;
    text.assign(data.begin() + 1, data.end());
    return Result<ByteBuffer>::ok(std::move(text));
}

// ---------------------------------------------------------------------------
// 0xF000 / 0xF001
// ---------------------------------------------------------------------------
ByteBuffer encodeUuidRecord(const Uuid &uuid) noexcept {
    ByteBuffer out;
    out.reserve(Uuid::kSize);
    out.assign(uuid.bytes.begin(), uuid.bytes.end());
    return out;
}

Result<Uuid> decodeUuidRecord(ConstBytes data) noexcept {
    if (data.size() != Uuid::kSize) {
        return wrongSize<Uuid>(static_cast<std::uint32_t>(data.size()));
    }
    Uuid uuid;
    if (!WireReader{data}.readArray<16>(uuid.bytes)) {
        return truncated<Uuid>(static_cast<std::uint32_t>(data.size()));
    }
    return Result<Uuid>::ok(uuid);
}

ByteBuffer encodeBoardIdRecord(const std::array<std::uint8_t, 8> &boardId) noexcept {
    ByteBuffer out;
    out.assign(boardId.begin(), boardId.end());
    return out;
}

Result<std::array<std::uint8_t, 8>> decodeBoardIdRecord(ConstBytes data) noexcept {
    using BoardId = std::array<std::uint8_t, 8>;
    if (data.size() != kBoardIdLength) {
        return wrongSize<BoardId>(static_cast<std::uint32_t>(data.size()));
    }
    BoardId boardId{};
    if (!WireReader{data}.readArray<8>(boardId)) {
        return truncated<BoardId>(static_cast<std::uint32_t>(data.size()));
    }
    return Result<BoardId>::ok(boardId);
}

// ---------------------------------------------------------------------------
// 0xF004
// ---------------------------------------------------------------------------
ByteBuffer encodeRecord(const ServerFirmwareVersionRecord &value) noexcept {
    ByteBuffer out;
    out.reserve(8);
    WireWriter w{out};
    w.writeU8(value.major);
    w.writeU8(value.minor);
    w.writeU16(value.patch);
    w.writeU32(value.build);
    return out;
}

Result<ServerFirmwareVersionRecord> decodeServerFirmwareVersionRecord(ConstBytes data) noexcept {
    using Record = ServerFirmwareVersionRecord;
    if (data.size() != 8) {
        return wrongSize<Record>(static_cast<std::uint32_t>(data.size()));
    }
    WireReader reader{data};
    Record value;
    if (!reader.readU8(value.major) || !reader.readU8(value.minor) || !reader.readU16(value.patch) ||
        !reader.readU32(value.build)) {
        return truncated<Record>(static_cast<std::uint32_t>(data.size()));
    }
    return Result<Record>::ok(value);
}

// ---------------------------------------------------------------------------
// 0xF006 / 0xF007 / 0xF008
// ---------------------------------------------------------------------------
ByteBuffer encodeRecord(const CapabilityDetectionRecord &value) noexcept {
    ByteBuffer out;
    out.reserve(16);
    WireWriter w{out};
    w.writeU64(value.knownMask);
    w.writeU64(value.detectedSupportedMask);
    return out;
}

Result<CapabilityDetectionRecord> decodeCapabilityDetectionRecord(ConstBytes data) noexcept {
    using Record = CapabilityDetectionRecord;
    if (data.size() != 16) {
        return wrongSize<Record>(static_cast<std::uint32_t>(data.size()));
    }
    WireReader reader{data};
    Record value;
    if (!reader.readU64(value.knownMask) || !reader.readU64(value.detectedSupportedMask)) {
        return truncated<Record>(static_cast<std::uint32_t>(data.size()));
    }
    return Result<Record>::ok(value);
}

ByteBuffer encodeRecord(const CapabilityOverrideRecord &value) noexcept {
    ByteBuffer out;
    out.reserve(16);
    WireWriter w{out};
    w.writeU64(value.forceOnMask);
    w.writeU64(value.forceOffMask);
    return out;
}

Result<CapabilityOverrideRecord> decodeCapabilityOverrideRecord(ConstBytes data) noexcept {
    using Record = CapabilityOverrideRecord;
    if (data.size() != 16) {
        return wrongSize<Record>(static_cast<std::uint32_t>(data.size()));
    }
    WireReader reader{data};
    Record value;
    if (!reader.readU64(value.forceOnMask) || !reader.readU64(value.forceOffMask)) {
        return truncated<Record>(static_cast<std::uint32_t>(data.size()));
    }
    // Specification §40.8: (forceOnMask & forceOffMask) == 0
    if (!capabilityOverrideValid(value.forceOnMask, value.forceOffMask)) {
        return Result<Record>::error(ProtocolErrorCode::InvalidField);
    }
    return Result<Record>::ok(value);
}

// ---------------------------------------------------------------------------
// 0xF009
// ---------------------------------------------------------------------------
ByteBuffer encodeRecord(const TimeoutConfigRecord &value) noexcept {
    ByteBuffer out;
    out.reserve(20);
    WireWriter w{out};
    w.writeU32(value.uartCommandTimeoutMs);
    w.writeU32(value.uartInterCommandGapMs);
    w.writeU32(value.sessionInactivityTimeoutMs);
    w.writeU32(value.tcpIdleTimeoutMs);
    w.writeU32(value.aliveCheckIntervalMs);
    return out;
}

Result<TimeoutConfigRecord> decodeTimeoutConfigRecord(ConstBytes data) noexcept {
    using Record = TimeoutConfigRecord;
    if (data.size() != 20) {
        return wrongSize<Record>(static_cast<std::uint32_t>(data.size()));
    }
    WireReader reader{data};
    Record value;
    if (!reader.readU32(value.uartCommandTimeoutMs) || !reader.readU32(value.uartInterCommandGapMs) ||
        !reader.readU32(value.sessionInactivityTimeoutMs) || !reader.readU32(value.tcpIdleTimeoutMs) ||
        !reader.readU32(value.aliveCheckIntervalMs)) {
        return truncated<Record>(static_cast<std::uint32_t>(data.size()));
    }
    return Result<Record>::ok(value);
}

bool timeoutConfigWithinBounds(const TimeoutConfigRecord &value) noexcept {
    return value.uartCommandTimeoutMs >= kMinUartCommandTimeoutMs &&
           value.uartCommandTimeoutMs <= kMaxUartCommandTimeoutMs &&
           value.uartInterCommandGapMs >= kMinUartInterCommandGapMs &&
           value.uartInterCommandGapMs <= kMaxUartInterCommandGapMs &&
           value.sessionInactivityTimeoutMs >= kMinSessionInactivityMs &&
           value.sessionInactivityTimeoutMs <= kMaxSessionInactivityMs &&
           value.tcpIdleTimeoutMs >= kMinTcpIdleTimeoutMs && value.tcpIdleTimeoutMs <= kMaxTcpIdleTimeoutMs &&
           value.aliveCheckIntervalMs >= kMinAliveCheckIntervalMs &&
           value.aliveCheckIntervalMs <= kMaxAliveCheckIntervalMs;
}

// ---------------------------------------------------------------------------
// 0xF00A
// ---------------------------------------------------------------------------
ByteBuffer encodeRecord(const ConnectionStatusRecord &value) noexcept {
    ByteBuffer out;
    out.reserve(8);
    WireWriter w{out};
    w.writeU8(value.controlOccupied);
    w.writeU8(value.activeObservers);
    w.writeU8(value.maxObservers);
    w.writeU8(value.activeSession);
    w.writeU16(value.controlClientLogicalAddress);
    w.writeU16(value.reserved);
    return out;
}

Result<ConnectionStatusRecord> decodeConnectionStatusRecord(ConstBytes data) noexcept {
    using Record = ConnectionStatusRecord;
    if (data.size() != 8) {
        return wrongSize<Record>(static_cast<std::uint32_t>(data.size()));
    }
    WireReader reader{data};
    Record value;
    if (!reader.readU8(value.controlOccupied) || !reader.readU8(value.activeObservers) ||
        !reader.readU8(value.maxObservers) || !reader.readU8(value.activeSession) ||
        !reader.readU16(value.controlClientLogicalAddress) || !reader.readU16(value.reserved)) {
        return truncated<Record>(static_cast<std::uint32_t>(data.size()));
    }
    if (value.reserved != 0) {
        return Result<Record>::error(ProtocolErrorCode::InvalidReservedBits, value.reserved);
    }
    return Result<Record>::ok(value);
}

// ---------------------------------------------------------------------------
// 0xF010
// ---------------------------------------------------------------------------
ByteBuffer encodeRecord(const UwbDeviceParametersRecord &value) noexcept {
    ByteBuffer out;
    out.reserve(8);
    WireWriter w{out};
    w.writeU16(value.id);
    w.writeU8(value.role);
    w.writeU8(value.channel);
    w.writeU16(value.rate);
    w.writeU16(value.reserved);
    return out;
}

Result<UwbDeviceParametersRecord> decodeUwbDeviceParametersRecord(ConstBytes data) noexcept {
    using Record = UwbDeviceParametersRecord;
    if (data.size() != 8) {
        return wrongSize<Record>(static_cast<std::uint32_t>(data.size()));
    }
    WireReader reader{data};
    Record value;
    if (!reader.readU16(value.id) || !reader.readU8(value.role) || !reader.readU8(value.channel) ||
        !reader.readU16(value.rate) || !reader.readU16(value.reserved)) {
        return truncated<Record>(static_cast<std::uint32_t>(data.size()));
    }
    if (value.reserved != 0) {
        return Result<Record>::error(ProtocolErrorCode::InvalidReservedBits, value.reserved);
    }
    return Result<Record>::ok(value);
}

// ---------------------------------------------------------------------------
// 0xF011
// ---------------------------------------------------------------------------
ByteBuffer encodeRecord(const UwbTwrParametersRecord &value) noexcept {
    ByteBuffer out;
    out.reserve(24);
    WireWriter w{out};
    w.writeU16(value.tagCapacity);
    w.writeU16(value.antennaDelay);
    w.writeU16(value.flags);
    w.writeU8(value.positioningDimension);
    w.writeU8(value.reserved);
    w.writeF32(value.kalmanQ);
    w.writeF32(value.kalmanR);
    w.writeF32(value.correctionParameterA);
    w.writeF32(value.correctionParameterB);
    return out;
}

Result<UwbTwrParametersRecord> decodeUwbTwrParametersRecord(ConstBytes data) noexcept {
    using Record = UwbTwrParametersRecord;
    if (data.size() != 24) {
        return wrongSize<Record>(static_cast<std::uint32_t>(data.size()));
    }
    WireReader reader{data};
    Record value;
    if (!reader.readU16(value.tagCapacity) || !reader.readU16(value.antennaDelay) || !reader.readU16(value.flags) ||
        !reader.readU8(value.positioningDimension) || !reader.readU8(value.reserved)) {
        return truncated<Record>(static_cast<std::uint32_t>(data.size()));
    }
    if (!reader.readF32(value.kalmanQ) || !reader.readF32(value.kalmanR) ||
        !reader.readF32(value.correctionParameterA) || !reader.readF32(value.correctionParameterB)) {
        return truncated<Record>(static_cast<std::uint32_t>(data.size()));
    }
    if ((value.flags & ~kTwrKnownFlags) != 0) {
        return Result<Record>::error(ProtocolErrorCode::InvalidReservedBits, value.flags);
    }
    if (value.reserved != 0) {
        return Result<Record>::error(ProtocolErrorCode::InvalidReservedBits, value.reserved);
    }
    return Result<Record>::ok(value);
}

// ---------------------------------------------------------------------------
// 0xF012
// ---------------------------------------------------------------------------
ByteBuffer encodeRecord(const UwbPdoaParametersRecord &value) noexcept {
    ByteBuffer out;
    out.reserve(24);
    WireWriter w{out};
    w.writeU16(value.dlist);
    w.writeU16(value.klist);
    w.writeU32(value.network);
    w.writeU16(value.anchorId);
    w.writeU16(value.rate);
    w.writeU8(value.filterEnabled);
    w.writeU8(value.reserved);
    w.writeU16(value.userCommand);
    w.writeI32(value.pdoaOffsetRaw);
    w.writeI32(value.rangeOffsetMm);
    return out;
}

Result<UwbPdoaParametersRecord> decodeUwbPdoaParametersRecord(ConstBytes data) noexcept {
    using Record = UwbPdoaParametersRecord;
    if (data.size() != 24) {
        return wrongSize<Record>(static_cast<std::uint32_t>(data.size()));
    }
    WireReader reader{data};
    Record value;
    if (!reader.readU16(value.dlist) || !reader.readU16(value.klist) || !reader.readU32(value.network) ||
        !reader.readU16(value.anchorId) || !reader.readU16(value.rate) || !reader.readU8(value.filterEnabled) ||
        !reader.readU8(value.reserved) || !reader.readU16(value.userCommand) ||
        !reader.readI32(value.pdoaOffsetRaw) || !reader.readI32(value.rangeOffsetMm)) {
        return truncated<Record>(static_cast<std::uint32_t>(data.size()));
    }
    if (value.reserved != 0) {
        return Result<Record>::error(ProtocolErrorCode::InvalidReservedBits, value.reserved);
    }
    return Result<Record>::ok(value);
}

// ---------------------------------------------------------------------------
// 0xF015
// ---------------------------------------------------------------------------
ByteBuffer encodeRecord(const LatestSensorDataRecord &value) noexcept {
    ByteBuffer out;
    out.reserve(16);
    WireWriter w{out};
    w.writeF32(value.accX);
    w.writeF32(value.accY);
    w.writeF32(value.accZ);
    w.writeF32(value.angle);
    return out;
}

Result<LatestSensorDataRecord> decodeLatestSensorDataRecord(ConstBytes data) noexcept {
    using Record = LatestSensorDataRecord;
    if (data.size() != 16) {
        return wrongSize<Record>(static_cast<std::uint32_t>(data.size()));
    }
    WireReader reader{data};
    Record value;
    if (!reader.readF32(value.accX) || !reader.readF32(value.accY) || !reader.readF32(value.accZ) ||
        !reader.readF32(value.angle)) {
        return truncated<Record>(static_cast<std::uint32_t>(data.size()));
    }
    return Result<Record>::ok(value);
}

// ---------------------------------------------------------------------------
// 0xF020
// ---------------------------------------------------------------------------
ByteBuffer encodeRecord(const DeviceCalibrationRecord &value) noexcept {
    ByteBuffer out;
    out.reserve(18);
    WireWriter w{out};
    w.writeU16(value.flags);
    w.writeI32(value.rangeScalePpm);
    w.writeI32(value.rangeOffsetMm);
    w.writeI32(value.azimuthOffsetMilliDeg);
    w.writeI32(value.elevationOffsetMilliDeg);
    return out;
}

Result<DeviceCalibrationRecord> decodeDeviceCalibrationRecord(ConstBytes data) noexcept {
    using Record = DeviceCalibrationRecord;
    if (data.size() != 18) {
        return wrongSize<Record>(static_cast<std::uint32_t>(data.size()));
    }
    WireReader reader{data};
    Record value;
    if (!reader.readU16(value.flags) || !reader.readI32(value.rangeScalePpm) ||
        !reader.readI32(value.rangeOffsetMm) || !reader.readI32(value.azimuthOffsetMilliDeg) ||
        !reader.readI32(value.elevationOffsetMilliDeg)) {
        return truncated<Record>(static_cast<std::uint32_t>(data.size()));
    }
    return Result<Record>::ok(value);
}

// ---------------------------------------------------------------------------
// Expected record sizes
// ---------------------------------------------------------------------------
std::optional<std::size_t> expectedDidRecordSize(Did did) noexcept {
    switch (did) {
    case Did::DeviceUuid: return 16;
    case Did::PicoBoardUniqueId: return 8;
    case Did::LogicalAddress: return 2;
    case Did::PicoFirmwareVersion: return 8;
    case Did::CapabilityDetection: return 16;
    case Did::CapabilityOverride: return 16;
    case Did::EffectiveCapabilities: return 8;
    case Did::TimeoutConfiguration: return 20;
    case Did::ConnectionStatus: return 8;
    case Did::Uptime: return 8;
    case Did::WifiRssi: return 2;
    case Did::UwbDeviceParameters: return 8;
    case Did::UwbTwrParameters: return 24;
    case Did::UwbPdoaParameters: return 24;
    case Did::UwbWorkMode: return 1;
    case Did::UwbMode: return 1;
    case Did::UwbLatestSensorData: return 16;
    case Did::UwbLatestDistance: return 4;
    case Did::DeviceCalibration: return 18;
    case Did::DeviceName:          // variable: u8 length prefix
    case Did::UwbModuleVersion:    // variable: u8 length prefix
    case Did::UwbMiscellaneousMetadata: // schemaVersion + TLV
    case Did::UwbCompleteConfiguration: // schemaVersion + entryCount + entries
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace uwb::protocol
