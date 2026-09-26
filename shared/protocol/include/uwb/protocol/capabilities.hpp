#pragma once

#include <cstdint>

namespace uwb::protocol {

// Specification §41 — 64-bit capability mask bits.
enum class Capability : std::uint64_t {
    Range = 1ULL << 0,
    PdoaAzimuth = 1ULL << 1,
    PdoaElevation = 1ULL << 2,
    UwbLocalPosition = 1ULL << 3,
    SensorData = 1ULL << 4,
    UwbConfigRead = 1ULL << 5,
    UwbConfigWrite = 1ULL << 6,
    TagManagement = 1ULL << 7,
    RawAt = 1ULL << 8,
    DeviceCalibration = 1ULL << 9,
    StreamLive = 1ULL << 10,
    StreamRecording = 1ULL << 11,
    MultiObserver = 1ULL << 12,
    SecurityAccess = 1ULL << 13,
    PersistentPicoConfig = 1ULL << 14,
    CompleteConfigWrite = 1ULL << 15,
    TimeSync = 1ULL << 16,
    OtaUpdate = 1ULL << 17,
};

using CapabilityMask = std::uint64_t;

[[nodiscard]] constexpr CapabilityMask operator|(Capability a, Capability b) noexcept {
    return static_cast<CapabilityMask>(a) | static_cast<CapabilityMask>(b);
}

[[nodiscard]] constexpr CapabilityMask operator|(CapabilityMask a, Capability b) noexcept {
    return a | static_cast<CapabilityMask>(b);
}

inline constexpr std::uint64_t kKnownCapabilityMask =
    Capability::Range | Capability::PdoaAzimuth | Capability::PdoaElevation | Capability::UwbLocalPosition |
    Capability::SensorData | Capability::UwbConfigRead | Capability::UwbConfigWrite | Capability::TagManagement |
    Capability::RawAt | Capability::DeviceCalibration | Capability::StreamLive | Capability::StreamRecording |
    Capability::MultiObserver | Capability::SecurityAccess | Capability::PersistentPicoConfig |
    Capability::CompleteConfigWrite | Capability::TimeSync | Capability::OtaUpdate;

[[nodiscard]] constexpr bool hasCapability(CapabilityMask mask, Capability capability) noexcept {
    return (mask & static_cast<CapabilityMask>(capability)) != 0;
}

// effective = (detected | forceOn) & ~forceOff   (specification §40.8)
[[nodiscard]] constexpr CapabilityMask applyCapabilityOverride(CapabilityMask detected,
                                                              CapabilityMask forceOn,
                                                              CapabilityMask forceOff) noexcept {
    return (detected | forceOn) & ~forceOff;
}

[[nodiscard]] constexpr bool capabilityOverrideValid(CapabilityMask forceOn, CapabilityMask forceOff) noexcept {
    return (forceOn & forceOff) == 0;
}

[[nodiscard]] constexpr std::uint32_t capabilityBitIndex(Capability capability) noexcept {
    auto value = static_cast<std::uint64_t>(capability);
    std::uint32_t index = 0;
    while ((value & 1ULL) == 0 && index < 64U) {
        value >>= 1U;
        ++index;
    }
    return index;
}

[[nodiscard]] const char *capabilityName(Capability capability) noexcept;

} // namespace uwb::protocol
