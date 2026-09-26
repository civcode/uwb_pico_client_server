#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "uwb/protocol/constants.hpp"
#include "uwb/protocol/did_records.hpp"
#include "uwb/protocol/dids.hpp"

#include "gen/golden_vectors.hpp"
#include "test_helpers.hpp"

using namespace uwb::protocol;
using uwb::test::sameBytes;
using uwb::test::view;

TEST_CASE("Record size expectations follow the §40 layout tables", "[protocol][did]") {
    struct Expectation {
        Did did;
        std::size_t size;
    };

    const Expectation expected[] = {
        {Did::DeviceUuid, 16},            // §40.1
        {Did::PicoBoardUniqueId, 8},      // §40.2
        {Did::LogicalAddress, 2},         // §40.4
        {Did::PicoFirmwareVersion, 8},    // §40.5
        {Did::CapabilityDetection, 16},   // §40.7
        {Did::CapabilityOverride, 16},    // §40.8
        {Did::EffectiveCapabilities, 8},  // §40.9
        {Did::TimeoutConfiguration, 20},  // §40.10
        {Did::ConnectionStatus, 8},       // §40.11
        {Did::Uptime, 8},                 // §40.12
        {Did::WifiRssi, 2},               // §40.13
        {Did::UwbDeviceParameters, 8},    // §40.14
        {Did::UwbTwrParameters, 24},      // §40.15
        {Did::UwbPdoaParameters, 24},     // §40.16
        {Did::UwbWorkMode, 1},            // §40.17
        {Did::UwbMode, 1},                // §40.18
        {Did::UwbLatestSensorData, 16},   // §40.19
        {Did::UwbLatestDistance, 4},      // §40.20
        {Did::DeviceCalibration, 18},     // §40.23
    };

    for (const Expectation &e : expected) {
        CHECK(expectedDidRecordSize(e.did) == e.size);
    }

    // Variable-length records have no fixed size.
    for (Did did : {Did::DeviceName, Did::UwbModuleVersion, Did::UwbMiscellaneousMetadata,
                    Did::UwbCompleteConfiguration}) {
        CHECK_FALSE(expectedDidRecordSize(did).has_value());
    }
}

TEST_CASE("Scalar records golden (§40.4, §40.12, §40.13, §40.16, §40.20)", "[protocol][did][golden]") {
    auto address = decodeU16Record(view(uwb::test::kRecordLogicalAddress));
    REQUIRE(address.ok());
    CHECK(*address == 0x1000);
    CHECK(sameBytes(bytesOf(encodeU16Record(0x1000)), view(uwb::test::kRecordLogicalAddress)));

    auto uptime = decodeU64Record(view(uwb::test::kRecordUptime));
    REQUIRE(uptime.ok());
    CHECK(*uptime == 1234567890123ULL);

    auto rssi = decodeI16Record(view(uwb::test::kRecordWifiRssi));
    REQUIRE(rssi.ok());
    CHECK(*rssi == -47);
    CHECK(sameBytes(bytesOf(encodeI16Record(-47)), view(uwb::test::kRecordWifiRssi)));

    auto distance = decodeI32Record(view(uwb::test::kRecordLatestDistance));
    REQUIRE(distance.ok());
    CHECK(*distance == 1234);

    CHECK(decodeU16Record(ConstBytes{}).failed());
    CHECK(decodeU16Record(bytesOf(std::array<std::uint8_t, 3>{1, 2, 3})).failed());
    CHECK(decodeI32Record(ConstBytes{}).failed());
}

TEST_CASE("Length-prefixed text records golden (§40.3, §40.6)", "[protocol][did][golden]") {
    auto name = decodeTextRecord8(view(uwb::test::kRecordDeviceName), kMaxDeviceNameLength);
    REQUIRE(name.ok());
    CHECK(name->size() == 11);
    CHECK(std::string(name->begin(), name->end()) == "pico-anchor");
    CHECK(sameBytes(bytesOf(encodeTextRecord8(name.value())), view(uwb::test::kRecordDeviceName)));

    auto version = decodeTextRecord8(view(uwb::test::kRecordUwbModuleVersion), 255);
    REQUIRE(version.ok());
    CHECK(std::string(version->begin(), version->end()) == "V1.0.0");

    // Empty name is allowed (§40.3).
    const std::array<std::uint8_t, 1> empty = {0x00};
    auto blank = decodeTextRecord8(view(empty), kMaxDeviceNameLength);
    REQUIRE(blank.ok());
    CHECK(blank->empty());

    // Over-long name is rejected.
    ByteBuffer longName;
    longName.push_back(33);
    longName.insert(longName.end(), 33, 'x');
    CHECK(decodeTextRecord8(bytesOf(longName), kMaxDeviceNameLength).failed());

    // Declared length must match the remaining bytes.
    const std::array<std::uint8_t, 3> truncated = {0x05, 'a', 'b'};
    CHECK(decodeTextRecord8(view(truncated), kMaxDeviceNameLength).failed());
    const std::array<std::uint8_t, 3> trailing = {0x01, 'a', 'b'};
    CHECK(decodeTextRecord8(view(trailing), kMaxDeviceNameLength).failed());
}

TEST_CASE("Firmware version record golden (§40.5)", "[protocol][did][golden]") {
    auto record = decodeServerFirmwareVersionRecord(view(uwb::test::kRecordServerFirmwareVersion));
    REQUIRE(record.ok());
    CHECK(record->major == 0);
    CHECK(record->minor == 1);
    CHECK(record->patch == 2);
    CHECK(record->build == 3);
    CHECK(sameBytes(bytesOf(encodeRecord(record.value())), view(uwb::test::kRecordServerFirmwareVersion)));
}

TEST_CASE("Capability records golden (§40.7 - §40.9)", "[protocol][did][golden]") {
    auto detection = decodeCapabilityDetectionRecord(view(uwb::test::kRecordCapabilityDetection));
    REQUIRE(detection.ok());
    CHECK(detection->knownMask == 0x000000000003FFFFULL);
    CHECK(detection->detectedSupportedMask == 0x000000000001FFFFULL);
    CHECK(sameBytes(bytesOf(encodeRecord(detection.value())), view(uwb::test::kRecordCapabilityDetection)));

    auto override_ = decodeCapabilityOverrideRecord(view(uwb::test::kRecordCapabilityOverride));
    REQUIRE(override_.ok());
    CHECK(override_->forceOnMask == 0);
    CHECK(override_->forceOffMask == 0x0000000000020000ULL);

    auto effective = decodeU64Record(view(uwb::test::kRecordEffectiveCapabilities));
    REQUIRE(effective.ok());
    // effective = (detected | forceOn) & ~forceOff  (§40.8)
    const CapabilityMask computed =
        (detection->detectedSupportedMask | override_->forceOnMask) & ~override_->forceOffMask;
    CHECK(*effective == computed);
}

TEST_CASE("Timeout configuration golden and §52 bounds", "[protocol][did][golden]") {
    auto record = decodeTimeoutConfigRecord(view(uwb::test::kRecordTimeoutConfig));
    REQUIRE(record.ok());
    CHECK(record->uartCommandTimeoutMs == 1000);
    CHECK(record->uartInterCommandGapMs == 100);
    CHECK(record->sessionInactivityTimeoutMs == 5000);
    CHECK(record->tcpIdleTimeoutMs == 60000);
    CHECK(record->aliveCheckIntervalMs == 10000);
    CHECK(sameBytes(bytesOf(encodeRecord(record.value())), view(uwb::test::kRecordTimeoutConfig)));

    CHECK(record->uartCommandTimeoutMs == kDefaultUartCommandTimeoutMs);
    CHECK(record->uartInterCommandGapMs == kDefaultUartInterCommandGapMs);
    CHECK(record->sessionInactivityTimeoutMs == kDefaultSessionInactivityMs);
    CHECK(record->tcpIdleTimeoutMs == kDefaultTcpIdleTimeoutMs);
    CHECK(record->aliveCheckIntervalMs == kDefaultAliveCheckIntervalMs);
    CHECK(timeoutConfigWithinBounds(record.value()));

    TimeoutConfigRecord tooFast = record.value();
    tooFast.uartCommandTimeoutMs = kMinUartCommandTimeoutMs - 1;
    CHECK_FALSE(timeoutConfigWithinBounds(tooFast));

    TimeoutConfigRecord tooIdle = record.value();
    tooIdle.tcpIdleTimeoutMs = kMaxTcpIdleTimeoutMs + 1;
    CHECK_FALSE(timeoutConfigWithinBounds(tooIdle));

    CHECK(decodeTimeoutConfigRecord(ConstBytes{uwb::test::kRecordTimeoutConfig.data(), 19}).failed());
}

TEST_CASE("Connection status record golden (§40.11)", "[protocol][did][golden]") {
    auto record = decodeConnectionStatusRecord(view(uwb::test::kRecordConnectionStatus));
    REQUIRE(record.ok());
    CHECK(record->controlOccupied == 1);
    CHECK(record->activeObservers == 2);
    CHECK(record->maxObservers == 4);
    CHECK(record->activeSession == 3);
    CHECK(record->controlClientLogicalAddress == 0x0E00);
    CHECK(record->reserved == 0);
    CHECK(sameBytes(bytesOf(encodeRecord(record.value())), view(uwb::test::kRecordConnectionStatus)));
}

TEST_CASE("UWB parameter records golden (§40.14 - §40.16)", "[protocol][did][golden]") {
    auto device = decodeUwbDeviceParametersRecord(view(uwb::test::kRecordUwbDeviceParameters));
    REQUIRE(device.ok());
    CHECK(device->id == 1);
    CHECK(device->role == 2); // AT+SETCFG role: 2 == anchor in the BU03/BU04 AT manual
    CHECK(device->channel == 5);
    CHECK(device->rate == 0);
    CHECK(sameBytes(bytesOf(encodeRecord(device.value())), view(uwb::test::kRecordUwbDeviceParameters)));

    auto twr = decodeUwbTwrParametersRecord(view(uwb::test::kRecordUwbTwrParameters));
    REQUIRE(twr.ok());
    CHECK(twr->tagCapacity == 64);
    CHECK(twr->antennaDelay == 0);
    CHECK(twr->flags == kTwrFlagKalmanEnabled);
    CHECK(twr->positioningDimension == 2);
    CHECK(twr->kalmanQ == 0.1F);
    CHECK(twr->kalmanR == 1.0F);
    CHECK(twr->correctionParameterA == 1.0F);
    CHECK(twr->correctionParameterB == 0.0F);
    CHECK(sameBytes(bytesOf(encodeRecord(twr.value())), view(uwb::test::kRecordUwbTwrParameters)));

    // §40.15 reserved flag bits.
    UwbTwrParametersRecord badFlags = twr.value();
    badFlags.flags = static_cast<std::uint16_t>(kTwrKnownFlags | 0x0400);
    CHECK(decodeUwbTwrParametersRecord(bytesOf(encodeRecord(badFlags))).failed());

    auto pdoa = decodeUwbPdoaParametersRecord(view(uwb::test::kRecordUwbPdoaParameters));
    REQUIRE(pdoa.ok());
    CHECK(pdoa->dlist == 0);
    CHECK(pdoa->klist == 0);
    CHECK(pdoa->network == 1);
    CHECK(pdoa->anchorId == 1);
    CHECK(pdoa->rate == 0);
    CHECK(pdoa->filterEnabled == 1);
    CHECK(pdoa->pdoaOffsetRaw == -45);
    CHECK(pdoa->rangeOffsetMm == -100);
    CHECK(sameBytes(bytesOf(encodeRecord(pdoa.value())), view(uwb::test::kRecordUwbPdoaParameters)));
}

TEST_CASE("Sensor data and calibration golden (§40.19, §40.23)", "[protocol][did][golden]") {
    auto sensor = decodeLatestSensorDataRecord(view(uwb::test::kRecordLatestSensorData));
    REQUIRE(sensor.ok());
    CHECK(sensor->accX == 0.01F);
    CHECK(sensor->accY == -0.02F);
    CHECK(sensor->accZ == 0.98F);
    CHECK(sensor->angle == 12.5F);
    CHECK(sameBytes(bytesOf(encodeRecord(sensor.value())), view(uwb::test::kRecordLatestSensorData)));

    auto calibration = decodeDeviceCalibrationRecord(view(uwb::test::kRecordDeviceCalibration));
    REQUIRE(calibration.ok());
    CHECK(calibration->flags == 0x001F);
    CHECK(calibration->rangeScalePpm == 1200);
    CHECK(calibration->rangeOffsetMm == -30);
    CHECK(calibration->azimuthOffsetMilliDeg == 1500);
    CHECK(calibration->elevationOffsetMilliDeg == -200);
    CHECK(sameBytes(bytesOf(encodeRecord(calibration.value())), view(uwb::test::kRecordDeviceCalibration)));
}

TEST_CASE("UUID and board id records golden (§40.1, §40.2)", "[protocol][did][golden]") {
    Uuid uuid;
    uuid.bytes = uwb::test::kDeviceUuid;
    ByteBuffer encoded = encodeUuidRecord(uuid);
    CHECK(sameBytes(bytesOf(encoded), ConstBytes{uuid.bytes.data(), uuid.bytes.size()}));

    auto decoded = decodeUuidRecord(bytesOf(encoded));
    REQUIRE(decoded.ok());
    CHECK(decoded->bytes == uuid.bytes);
    CHECK(decodeUuidRecord(ConstBytes{encoded.data(), 15}).failed());

    auto boardId = decodeBoardIdRecord(view(uwb::test::kBoardId));
    REQUIRE(boardId.ok());
    CHECK(boardId.value() == uwb::test::kBoardId);
    CHECK(sameBytes(bytesOf(encodeBoardIdRecord(boardId.value())), view(uwb::test::kBoardId)));
}

TEST_CASE("DID registry (§39)", "[protocol][did]") {
    CHECK(isKnownDid(0xF000));
    CHECK(isKnownDid(0xF020));
    CHECK_FALSE(isKnownDid(0xF021));
    CHECK_FALSE(isKnownDid(0x0001));
    CHECK(isInDidNamespace(0xF0FF));
    CHECK_FALSE(isInDidNamespace(0x0F00));

    CHECK(didAccess(Did::DeviceName) == DidAccess::ReadWrite);
    CHECK(didAccess(Did::Uptime) == DidAccess::ReadOnly);
    CHECK(didPersistence(Did::TimeoutConfiguration) == DidPersistence::PicoPersistent);
    CHECK(didPersistence(Did::UwbDeviceParameters) == DidPersistence::UwbRuntime);
    CHECK(std::string{didName(Did::UwbCompleteConfiguration)} == "UwbCompleteConfiguration");
}

TEST_CASE("Logical address ranges (§40.4)", "[protocol][did]") {
    CHECK(isClientLogicalAddress(0x0E00));
    CHECK(isClientLogicalAddress(0x0EFF));
    CHECK_FALSE(isClientLogicalAddress(0x0F00));
    CHECK(isDeviceLogicalAddress(0x1000));
    CHECK(isDeviceLogicalAddress(0xEFFF));
    CHECK_FALSE(isDeviceLogicalAddress(0xF000));
    CHECK_FALSE(isClientLogicalAddress(kLogicalAddressInvalid));
    CHECK_FALSE(isDeviceLogicalAddress(kLogicalAddressBroadcast));
}

TEST_CASE("didRecordSize matches the section 40 record layouts", "[protocol][did]") {
    using Size = std::optional<std::size_t>;
    CHECK(didRecordSize(Did::DeviceUuid) == Size{16});
    CHECK(uwb::test::kDeviceUuid.size() == didRecordSize(Did::DeviceUuid).value());
    CHECK(uwb::test::kBoardId.size() == didRecordSize(Did::PicoBoardUniqueId).value());
    CHECK(uwb::test::kRecordLogicalAddress.size() == didRecordSize(Did::LogicalAddress).value());
    CHECK(uwb::test::kRecordServerFirmwareVersion.size() == didRecordSize(Did::PicoFirmwareVersion).value());
    CHECK(uwb::test::kRecordCapabilityDetection.size() == didRecordSize(Did::CapabilityDetection).value());
    CHECK(uwb::test::kRecordCapabilityOverride.size() == didRecordSize(Did::CapabilityOverride).value());
    CHECK(uwb::test::kRecordEffectiveCapabilities.size() == didRecordSize(Did::EffectiveCapabilities).value());
    CHECK(uwb::test::kRecordTimeoutConfig.size() == didRecordSize(Did::TimeoutConfiguration).value());
    CHECK(uwb::test::kRecordConnectionStatus.size() == didRecordSize(Did::ConnectionStatus).value());
    CHECK(uwb::test::kRecordUptime.size() == didRecordSize(Did::Uptime).value());
    CHECK(uwb::test::kRecordWifiRssi.size() == didRecordSize(Did::WifiRssi).value());
    CHECK(uwb::test::kRecordUwbDeviceParameters.size() == didRecordSize(Did::UwbDeviceParameters).value());
    CHECK(uwb::test::kRecordUwbTwrParameters.size() == didRecordSize(Did::UwbTwrParameters).value());
    CHECK(uwb::test::kRecordUwbPdoaParameters.size() == didRecordSize(Did::UwbPdoaParameters).value());
    CHECK(uwb::test::kRecordUwbWorkMode.size() == didRecordSize(Did::UwbWorkMode).value());
    CHECK(uwb::test::kRecordUwbMode.size() == didRecordSize(Did::UwbMode).value());
    CHECK(uwb::test::kRecordLatestSensorData.size() == didRecordSize(Did::UwbLatestSensorData).value());
    CHECK(uwb::test::kRecordLatestDistance.size() == didRecordSize(Did::UwbLatestDistance).value());
    CHECK(uwb::test::kRecordDeviceCalibration.size() == didRecordSize(Did::DeviceCalibration).value());

    // Text records and TLV containers are variable length.
    CHECK_FALSE(didRecordSize(Did::DeviceName).has_value());
    CHECK_FALSE(didRecordSize(Did::UwbModuleVersion).has_value());
    CHECK_FALSE(didRecordSize(Did::UwbMiscellaneousMetadata).has_value());
    CHECK_FALSE(didRecordSize(Did::UwbCompleteConfiguration).has_value());
}
