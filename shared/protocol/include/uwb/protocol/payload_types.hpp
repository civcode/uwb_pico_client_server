#pragma once

#include <cstdint>

namespace uwb::protocol {

// Specification §14.
enum class PayloadType : std::uint16_t {
    GenericHeaderNack = 0x0000,
    DeviceIdRequest = 0x0001,
    DeviceIdResponse = 0x0004,
    ConnectionActivationRequest = 0x0005,
    ConnectionActivationResponse = 0x0006,
    AliveCheckRequest = 0x0007,
    AliveCheckResponse = 0x0008,
    ApplicationMessage = 0x8001,
    ApplicationMessageAck = 0x8002,
    ApplicationMessageNack = 0x8003,
    EventNotification = 0x8010,
};

[[nodiscard]] constexpr bool isKnownPayloadType(std::uint16_t value) noexcept {
    switch (value) {
    case 0x0000:
    case 0x0001:
    case 0x0004:
    case 0x0005:
    case 0x0006:
    case 0x0007:
    case 0x0008:
    case 0x8001:
    case 0x8002:
    case 0x8003:
    case 0x8010:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] constexpr bool isKnownPayloadType(PayloadType type) noexcept {
    return isKnownPayloadType(static_cast<std::uint16_t>(type));
}

// Payload types restricted to UDP discovery in v1 (specification §8, §14).
[[nodiscard]] constexpr bool isUdpPayloadType(PayloadType type) noexcept {
    return type == PayloadType::DeviceIdRequest || type == PayloadType::DeviceIdResponse;
}

[[nodiscard]] constexpr bool isTcpOnlyPayloadType(PayloadType type) noexcept {
    switch (type) {
    case PayloadType::DeviceIdRequest:
    case PayloadType::DeviceIdResponse:
        return false;
    case PayloadType::ConnectionActivationRequest:
    case PayloadType::ConnectionActivationResponse:
    case PayloadType::AliveCheckRequest:
    case PayloadType::AliveCheckResponse:
    case PayloadType::ApplicationMessage:
    case PayloadType::ApplicationMessageAck:
    case PayloadType::ApplicationMessageNack:
    case PayloadType::EventNotification:
        return true;
    case PayloadType::GenericHeaderNack:
        return false; // valid on both transports
    }
    return false;
}

[[nodiscard]] const char *toString(PayloadType type) noexcept;

} // namespace uwb::protocol
