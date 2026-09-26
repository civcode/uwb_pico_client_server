#include "uwb/server/did_service.hpp"

#include "uwb/protocol/capabilities.hpp"
#include "uwb/protocol/constants.hpp"
#include "uwb/protocol/errors.hpp"

namespace uwb::server {

namespace {
// §27: structurally invalid data maps to 0x13; data that is structurally valid
// but semantically outside the allowed range maps to 0x31.
[[nodiscard]] ServerStatus recordDecodeStatus(uwb::protocol::ProtocolErrorCode code) noexcept {
    return code == uwb::protocol::ProtocolErrorCode::InvalidField ? ServerStatus::InvalidAddress
                                                                  : ServerStatus::InvalidRecord;
}
} // namespace

using uwb::protocol::CapabilityDetectionRecord;
using uwb::protocol::CapabilityOverrideRecord;
using uwb::protocol::DidAccess;
using uwb::protocol::DidPersistence;
using uwb::protocol::Result;

DidService::DidService(const ServerConfig &config, IConfigurationStorage &storage) noexcept
    : config_(config), storage_(storage) {}

void DidService::setDeviceIdentity(const Uuid &deviceUuid, const std::array<std::uint8_t, 8> &boardUniqueId,
                                   std::string deviceName, std::uint16_t logicalAddress,
                                   std::uint16_t tcpPort) noexcept {
    deviceUuid_ = deviceUuid;
    boardUniqueId_ = boardUniqueId;
    deviceName_ = std::move(deviceName);
    if (deviceName_.size() > uwb::protocol::kMaxDeviceNameLength) {
        deviceName_.resize(uwb::protocol::kMaxDeviceNameLength); // §40.3
    }
    logicalAddress_ = logicalAddress;
    tcpPort_ = tcpPort;
}

void DidService::setFirmwareVersion(uwb::protocol::ServerFirmwareVersionRecord version) noexcept {
    firmware_ = version;
}

void DidService::setCapabilityOverride(CapabilityOverrideRecord capabilityOverride) noexcept {
    capabilityOverride_ = capabilityOverride;
}

void DidService::setCapabilityMasks(uwb::protocol::CapabilityMask known, uwb::protocol::CapabilityMask detected) noexcept {
    knownMask_ = known;
    detectedMask_ = detected;
}

void DidService::setTimeouts(TimeoutSettings timeouts) noexcept { timeouts_ = timeouts; }
void DidService::setUptime(std::uint64_t uptimeUs) noexcept { uptimeUs_ = uptimeUs; }
void DidService::setWifiRssi(std::int16_t rssiDbm) noexcept { wifiRssiDbm_ = rssiDbm; }
void DidService::setLatestDistance(std::int32_t distanceMm) noexcept { latestDistanceMm_ = distanceMm; }
void DidService::setLatestSensor(uwb::protocol::LatestSensorDataRecord record) noexcept { latestSensor_ = record; }
void DidService::setConnectionStatus(uwb::protocol::ConnectionStatusRecord status) noexcept {
    connectionStatus_ = status;
}

uwb::protocol::CapabilityMask DidService::effectiveCapabilities() const noexcept {
    return uwb::protocol::applyCapabilityOverride(detectedMask_, capabilityOverride_.forceOnMask,
                                                  capabilityOverride_.forceOffMask);
}

// DIDs whose values live in the UWB module: the server core must ask the backend
// (specification §26 async read, §48).
static bool didNeedsBackend(Did did) noexcept {
    switch (did) {
    case Did::UwbModuleVersion:
    case Did::UwbDeviceParameters:
    case Did::UwbTwrParameters:
    case Did::UwbPdoaParameters:
    case Did::UwbWorkMode:
    case Did::UwbMode:
    case Did::UwbMiscellaneousMetadata:
    case Did::UwbCompleteConfiguration:
    case Did::DeviceCalibration:
        return true;
    default:
        return false;
    }
}

ServerResult<ByteBuffer> DidService::read(Did did) const {
    if (!uwb::protocol::isKnownDid(did)) {
        return ServerResult<ByteBuffer>::err(ServerStatus::UnknownDid); // §26 -> NRC 0x31
    }
    if (didNeedsBackend(did)) {
        return ServerResult<ByteBuffer>::err(ServerStatus::NeedsBackend);
    }

    switch (did) {
    case Did::DeviceUuid:
        return ServerResult<ByteBuffer>::okResult(uwb::protocol::encodeUuidRecord(deviceUuid_));
    case Did::PicoBoardUniqueId:
        return ServerResult<ByteBuffer>::okResult(uwb::protocol::encodeBoardIdRecord(boardUniqueId_));
    case Did::DeviceName: {
        ByteBuffer name(deviceName_.begin(), deviceName_.end());
        return ServerResult<ByteBuffer>::okResult(uwb::protocol::encodeTextRecord8(name));
    }
    case Did::LogicalAddress:
        return ServerResult<ByteBuffer>::okResult(uwb::protocol::encodeU16Record(logicalAddress_));
    case Did::PicoFirmwareVersion:
        return ServerResult<ByteBuffer>::okResult(uwb::protocol::encodeRecord(firmware_));
    case Did::CapabilityDetection: {
        CapabilityDetectionRecord record;
        record.knownMask = knownMask_;
        record.detectedSupportedMask = detectedMask_;
        return ServerResult<ByteBuffer>::okResult(uwb::protocol::encodeRecord(record));
    }
    case Did::CapabilityOverride:
        return ServerResult<ByteBuffer>::okResult(uwb::protocol::encodeRecord(capabilityOverride_));
    case Did::EffectiveCapabilities:
        return ServerResult<ByteBuffer>::okResult(uwb::protocol::encodeU64Record(effectiveCapabilities()));
    case Did::TimeoutConfiguration:
        return ServerResult<ByteBuffer>::okResult(
            uwb::protocol::encodeRecord(toTimeoutRecord(timeouts_)));
    case Did::ConnectionStatus:
        return ServerResult<ByteBuffer>::okResult(uwb::protocol::encodeRecord(connectionStatus_));
    case Did::Uptime:
        return ServerResult<ByteBuffer>::okResult(uwb::protocol::encodeU64Record(uptimeUs_));
    case Did::WifiRssi:
        return ServerResult<ByteBuffer>::okResult(uwb::protocol::encodeI16Record(wifiRssiDbm_));
    case Did::UwbLatestSensorData:
        return ServerResult<ByteBuffer>::okResult(uwb::protocol::encodeRecord(latestSensor_));
    case Did::UwbLatestDistance:
        return ServerResult<ByteBuffer>::okResult(uwb::protocol::encodeI32Record(latestDistanceMm_));
    default:
        return ServerResult<ByteBuffer>::err(ServerStatus::UnsupportedDid);
    }
}

// Role/session/access policy for a write (specification §9.2, §22.3, §39).
ServerStatus DidService::checkWritePolicy(Did did, const ConnectionContext &ctx) const {
    if (!uwb::protocol::isKnownDid(did)) {
        return ServerStatus::UnknownDid;
    }
    if (uwb::protocol::didAccess(did) != DidAccess::ReadWrite) {
        return ServerStatus::ReadOnlyDid; // §39 read-only DID
    }
    if (!ctx.isActive() || !ctx.isControl()) {
        return ServerStatus::RoleDenied; // §9.2: Observers cannot write DIDs
    }
    if (!ctx.isExtendedSession()) {
        return ServerStatus::SessionDenied; // §22.3: writes need Extended Session
    }
    if (config_.securityEnabled && !ctx.isUnlocked()) {
        return ServerStatus::SecurityDenied; // §25
    }
    return ServerStatus::Ok;
}

// Structural validation of the record against the §40 layout.
ServerStatus DidService::checkRecord(Did did, ConstBytes record) const {
    const auto expected = uwb::protocol::didRecordSize(did);
    if (expected.has_value() && record.size() != *expected) {
        return ServerStatus::InvalidRecord; // §27 -> NRC 0x13
    }

    switch (did) {
    case Did::TimeoutConfiguration: {
        auto decoded = uwb::protocol::decodeTimeoutConfigRecord(record);
        if (!decoded.ok()) {
            return recordDecodeStatus(decoded.code());
        }
        if (!uwb::protocol::timeoutConfigWithinBounds(decoded.value())) {
            return ServerStatus::InvalidAddress; // out of the §52 range -> NRC 0x31
        }
        return ServerStatus::Ok;
    }
    case Did::CapabilityOverride: {
        auto decoded = uwb::protocol::decodeCapabilityOverrideRecord(record);
        if (!decoded.ok()) {
            return recordDecodeStatus(decoded.code());
        }
        // §40.8 validation rule.
        if ((decoded.value().forceOnMask & decoded.value().forceOffMask) != 0) {
            return ServerStatus::InvalidAddress;
        }
        return ServerStatus::Ok;
    }
    case Did::LogicalAddress: {
        auto decoded = uwb::protocol::decodeU16Record(record);
        if (!decoded.ok()) {
            return recordDecodeStatus(decoded.code());
        }
        // §40.4: device writes must stay in the Pico device range.
        if (!uwb::protocol::isDeviceLogicalAddress(decoded.value())) {
            return ServerStatus::InvalidAddress;
        }
        return ServerStatus::Ok;
    }
    case Did::DeviceName: {
        auto decoded = uwb::protocol::decodeTextRecord8(record, uwb::protocol::kMaxDeviceNameLength);
        return decoded.ok() ? ServerStatus::Ok : recordDecodeStatus(decoded.code());
    }
    case Did::UwbLatestSensorData: {
        auto decoded = uwb::protocol::decodeLatestSensorDataRecord(record);
        return decoded.ok() ? ServerStatus::Ok : recordDecodeStatus(decoded.code());
    }
    case Did::DeviceCalibration: {
        auto decoded = uwb::protocol::decodeDeviceCalibrationRecord(record);
        return decoded.ok() ? ServerStatus::Ok : recordDecodeStatus(decoded.code());
    }
    case Did::UwbDeviceParameters: {
        auto decoded = uwb::protocol::decodeUwbDeviceParametersRecord(record);
        return decoded.ok() ? ServerStatus::Ok : recordDecodeStatus(decoded.code());
    }
    case Did::UwbTwrParameters: {
        auto decoded = uwb::protocol::decodeUwbTwrParametersRecord(record);
        return decoded.ok() ? ServerStatus::Ok : recordDecodeStatus(decoded.code());
    }
    case Did::UwbPdoaParameters: {
        auto decoded = uwb::protocol::decodeUwbPdoaParametersRecord(record);
        return decoded.ok() ? ServerStatus::Ok : recordDecodeStatus(decoded.code());
    }
    default:
        return ServerStatus::Ok;
    }
}

ServerResult<bool> DidService::write(ConnectionId connection, const ConnectionContext &ctx, Did did,
                                     ConstBytes record) {
    (void)connection;

    const ServerStatus policy = checkWritePolicy(did, ctx);
    if (policy != ServerStatus::Ok) {
        ++rejectedWrites_;
        return ServerResult<bool>::err(policy);
    }

    if (didNeedsBackend(did)) {
        // UWB-backed writes are executed through IUwbBackend by the dispatcher
        // (§27, §48); the policy check above is shared by both paths.
        return ServerResult<bool>::err(ServerStatus::NeedsBackend);
    }

    const ServerStatus structural = checkRecord(did, record);
    if (structural != ServerStatus::Ok) {
        ++rejectedWrites_;
        return ServerResult<bool>::err(structural);
    }

    const DidPersistence persistence = uwb::protocol::didPersistence(did);
    if (persistence != DidPersistence::PicoPersistent) {
        ++rejectedWrites_;
        return ServerResult<bool>::err(ServerStatus::ReadOnlyDid);
    }

    // Snapshot for rollback: §27 requires an atomic commit. A failed storage
    // commit must leave the previously active value in effect.
    const TimeoutSettings previousTimeouts = timeouts_;
    const CapabilityOverrideRecord previousOverride = capabilityOverride_;
    const std::string previousName = deviceName_;
    const std::uint16_t previousAddress = logicalAddress_;

    auto rollback = [&]() noexcept {
        timeouts_ = previousTimeouts;
        capabilityOverride_ = previousOverride;
        deviceName_ = previousName;
        logicalAddress_ = previousAddress;
    };

    switch (did) {
    case Did::TimeoutConfiguration: {
        auto decoded = uwb::protocol::decodeTimeoutConfigRecord(record);
        timeouts_ = fromTimeoutRecord(decoded.value());
        break;
    }
    case Did::CapabilityOverride: {
        auto decoded = uwb::protocol::decodeCapabilityOverrideRecord(record);
        capabilityOverride_ = decoded.value();
        break;
    }
    case Did::DeviceName: {
        auto decoded = uwb::protocol::decodeTextRecord8(record, uwb::protocol::kMaxDeviceNameLength);
        deviceName_.assign(decoded.value().begin(), decoded.value().end());
        break;
    }
    case Did::LogicalAddress: {
        auto decoded = uwb::protocol::decodeU16Record(record);
        logicalAddress_ = decoded.value();
        break;
    }
    default:
        rollback();
        ++rejectedWrites_;
        return ServerResult<bool>::err(ServerStatus::UnsupportedDid);
    }

    auto staged = storage_.stage(record);
    if (!staged.ok()) {
        rollback();
        ++rejectedWrites_;
        return ServerResult<bool>::err(ServerStatus::StorageUnavailable);
    }
    auto committed = storage_.commit();
    if (!committed.ok()) {
        storage_.discardStaged();
        rollback();
        ++rejectedWrites_;
        return ServerResult<bool>::err(ServerStatus::StorageUnavailable);
    }

    ++writes_;
    return ServerResult<bool>::okResult(true);
}

} // namespace uwb::server
