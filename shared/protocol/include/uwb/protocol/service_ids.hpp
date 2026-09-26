#pragma once

#include <cstdint>

#include "uwb/protocol/constants.hpp"

namespace uwb::protocol {

// UDS-inspired service identifiers (specification §23..§31).
enum class ServiceId : std::uint8_t {
    SessionControl = 0x10,
    PicoDeviceReset = 0x11,
    ReadDataByIdentifier = 0x22,
    SecurityAccess = 0x27,
    WriteDataByIdentifier = 0x2E,
    RoutineControl = 0x31,
    ClientPresent = 0x3E,
    ExecuteAtCommand = 0x40,
    EventControl = 0x41,
};

[[nodiscard]] constexpr bool isKnownServiceId(std::uint8_t value) noexcept {
    switch (value) {
    case 0x10: case 0x11: case 0x22: case 0x27: case 0x2E:
    case 0x31: case 0x3E: case 0x40: case 0x41:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] constexpr std::uint8_t positiveResponseSid(std::uint8_t requestSid) noexcept {
    return static_cast<std::uint8_t>(requestSid + kPositiveResponseSidOffset);
}

// Session identifiers (specification §22.1).
enum class SessionId : std::uint8_t {
    Default = 0x01,
    Extended = 0x03,
};

[[nodiscard]] constexpr bool isKnownSessionId(std::uint8_t value) noexcept {
    return value == 0x01 || value == 0x03;
}

// Connection roles (specification §18.1).
enum class ConnectionRole : std::uint8_t {
    None = 0x00,
    Control = 0x01,
    Observer = 0x02,
};

[[nodiscard]] constexpr bool isKnownConnectionRole(std::uint8_t value) noexcept {
    return value == 0x01 || value == 0x02;
}

// Connection Activation response codes (specification §18.2).
enum class ActivationResponseCode : std::uint8_t {
    AcceptedRequestedRole = 0x00,
    AcceptedDowngradedToObserver = 0x01,
    RejectedNoConnectionSlot = 0x10,
    RejectedRolePolicyDenied = 0x11,
    RejectedIncompatibleMajorVersion = 0x12,
    RejectedServerNotReady = 0x13,
};

[[nodiscard]] constexpr bool isActivationRejection(ActivationResponseCode code) noexcept {
    switch (code) {
    case ActivationResponseCode::RejectedNoConnectionSlot:
    case ActivationResponseCode::RejectedRolePolicyDenied:
    case ActivationResponseCode::RejectedIncompatibleMajorVersion:
    case ActivationResponseCode::RejectedServerNotReady:
        return true;
    case ActivationResponseCode::AcceptedRequestedRole:
    case ActivationResponseCode::AcceptedDowngradedToObserver:
        return false;
    }
    return true;
}

// Control status byte of the Device Identification Response (specification §17).
enum class ControlStatus : std::uint8_t {
    Available = 0x00,
    Occupied = 0x01,
};

// Service 0x11 sub-functions (specification §24).
enum class DeviceResetType : std::uint8_t {
    Hard = 0x01,
    Soft = 0x03,
};

[[nodiscard]] constexpr bool isKnownDeviceResetType(std::uint8_t value) noexcept {
    return value == 0x01 || value == 0x03;
}

// Service 0x27 sub-functions (specification §25).
enum class SecuritySubFunction : std::uint8_t {
    RequestSeed = 0x01,
    SendKey = 0x02,
};

[[nodiscard]] constexpr bool isKnownSecuritySubFunction(std::uint8_t value) noexcept {
    return value == 0x01 || value == 0x02;
}

// Service 0x41 sub-functions (specification §31.1).
enum class EventControlSubFunction : std::uint8_t {
    Subscribe = 0x01,
    Unsubscribe = 0x02,
    QuerySubscription = 0x03,
    UnsubscribeAll = 0x04,
};

[[nodiscard]] constexpr bool isKnownEventControlSubFunction(std::uint8_t value) noexcept {
    return value >= 0x01 && value <= 0x04;
}

// Service 0x10 default timing values (specification §23).
inline constexpr std::uint16_t kDefaultP2ServerMaxMs = 1000;
inline constexpr std::uint16_t kDefaultP2StarServerMax10ms = 500;

// Application Message flags (specification §20.1).
inline constexpr std::uint8_t kApplicationFlagAckRequired = 1U << 0U;
inline constexpr std::uint8_t kApplicationFlagReservedMask = 0xFEU;

} // namespace uwb::protocol
