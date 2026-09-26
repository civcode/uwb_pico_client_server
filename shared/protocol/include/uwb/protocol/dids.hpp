#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace uwb::protocol {

// Specification §39 — the single DID registry. Server and client code must not
// duplicate these as magic numbers.
enum class Did : std::uint16_t {
    DeviceUuid = 0xF000,
    PicoBoardUniqueId = 0xF001,
    DeviceName = 0xF002,
    LogicalAddress = 0xF003,
    PicoFirmwareVersion = 0xF004,
    UwbModuleVersion = 0xF005,
    CapabilityDetection = 0xF006,
    CapabilityOverride = 0xF007,
    EffectiveCapabilities = 0xF008,
    TimeoutConfiguration = 0xF009,
    ConnectionStatus = 0xF00A,
    Uptime = 0xF00B,
    WifiRssi = 0xF00C,
    UwbDeviceParameters = 0xF010,
    UwbTwrParameters = 0xF011,
    UwbPdoaParameters = 0xF012,
    UwbWorkMode = 0xF013,
    UwbMode = 0xF014,
    UwbLatestSensorData = 0xF015,
    UwbLatestDistance = 0xF016,
    UwbMiscellaneousMetadata = 0xF017,
    UwbCompleteConfiguration = 0xF01F,
    DeviceCalibration = 0xF020,
};

enum class DidAccess : std::uint8_t {
    ReadOnly,
    ReadWrite,
};

enum class DidPersistence : std::uint8_t {
    Derived,
    Firmware,
    PicoPersistent,
    UwbRuntime,        // runtime; Save routine persists
    UwbConditional,    // runtime; saved if the module supports it
    UwbOrPico,         // backend dependent
    Runtime,
};

[[nodiscard]] constexpr bool isKnownDid(std::uint16_t value) noexcept {
    switch (value) {
    case 0xF000: case 0xF001: case 0xF002: case 0xF003: case 0xF004: case 0xF005:
    case 0xF006: case 0xF007: case 0xF008: case 0xF009: case 0xF00A: case 0xF00B:
    case 0xF00C: case 0xF010: case 0xF011: case 0xF012: case 0xF013: case 0xF014:
    case 0xF015: case 0xF016: case 0xF017: case 0xF01F: case 0xF020:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] constexpr bool isKnownDid(Did did) noexcept { return isKnownDid(static_cast<std::uint16_t>(did)); }

[[nodiscard]] constexpr bool isInDidNamespace(std::uint16_t value) noexcept {
    return value >= 0xF000 && value <= 0xF0FF;
}

[[nodiscard]] DidAccess didAccess(Did did) noexcept;
[[nodiscard]] DidPersistence didPersistence(Did did) noexcept;
[[nodiscard]] const char *didName(Did did) noexcept;

// Expected record length of each fixed-size DID, taken from the section 40
// record layouts.  Text records (0xF002, 0xF005) and TLV containers (0xF017,
// 0xF01F) have a variable length and report no fixed size.
[[nodiscard]] std::optional<std::size_t> didRecordSize(Did did) noexcept;

// Logical address ranges (specification §40.4).
inline constexpr std::uint16_t kLogicalAddressInvalid = 0x0000;
inline constexpr std::uint16_t kLogicalAddressBroadcast = 0xFFFF;
inline constexpr std::uint16_t kClientLogicalAddressMin = 0x0E00;
inline constexpr std::uint16_t kClientLogicalAddressMax = 0x0EFF;
inline constexpr std::uint16_t kDeviceLogicalAddressMin = 0x1000;
inline constexpr std::uint16_t kDeviceLogicalAddressMax = 0xEFFF;

[[nodiscard]] constexpr bool isClientLogicalAddress(std::uint16_t address) noexcept {
    return address >= kClientLogicalAddressMin && address <= kClientLogicalAddressMax;
}

[[nodiscard]] constexpr bool isDeviceLogicalAddress(std::uint16_t address) noexcept {
    return address >= kDeviceLogicalAddressMin && address <= kDeviceLogicalAddressMax;
}

[[nodiscard]] constexpr bool isUsableLogicalAddress(std::uint16_t address) noexcept {
    return address != kLogicalAddressInvalid && address != kLogicalAddressBroadcast;
}

} // namespace uwb::protocol
