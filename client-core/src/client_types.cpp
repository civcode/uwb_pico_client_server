#include "uwb/client/client_types.hpp"

#include <string>

namespace uwb::client {

namespace {

using uwb::protocol::Capability;

[[nodiscard]] bool parsePort(const std::string &text, std::uint16_t &port) noexcept {
    if (text.empty() || text.size() > 5) {
        return false;
    }
    unsigned long value = 0;
    for (char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        value = value * 10UL + static_cast<unsigned long>(ch - '0');
    }
    if (value > 65535UL) {
        return false;
    }
    port = static_cast<std::uint16_t>(value);
    return true;
}

} // namespace

std::string Endpoint::toString() const {
    if (host.find(':') != std::string::npos) {
        return "[" + host + "]:" + std::to_string(port);
    }
    return host + ":" + std::to_string(port);
}

std::optional<Endpoint> parseEndpoint(std::string_view view) noexcept {
    const std::string text(view);
    Endpoint endpoint;

    // IPv6 form: [::1]:13401
    if (!text.empty() && text.front() == '[') {
        const auto close = text.find(']');
        if (close == std::string::npos || close + 1 >= text.size() || text[close + 1] != ':') {
            return std::nullopt;
        }
        endpoint.host = text.substr(1, close - 1);
        if (endpoint.host.empty() || !parsePort(text.substr(close + 2), endpoint.port)) {
            return std::nullopt;
        }
        return endpoint;
    }

    const auto colon = text.rfind(':');
    if (colon == std::string::npos) {
        return std::nullopt;
    }
    endpoint.host = text.substr(0, colon);
    if (endpoint.host.empty() || !parsePort(text.substr(colon + 1), endpoint.port)) {
        return std::nullopt;
    }
    return endpoint;
}

const char *connectionStateName(ConnectionState state) noexcept {
    switch (state) {
    case ConnectionState::Disconnected:
        return "disconnected";
    case ConnectionState::Connecting:
        return "connecting";
    case ConnectionState::TcpConnected:
        return "tcp-connected";
    case ConnectionState::Activated:
        return "activated";
    case ConnectionState::Ready:
        return "ready";
    case ConnectionState::ReconnectWait:
        return "reconnect-wait";
    case ConnectionState::Closing:
        return "closing";
    }
    return "unknown";
}

std::string roleName(uwb::protocol::ConnectionRole role) {
    switch (role) {
    case uwb::protocol::ConnectionRole::Observer:
        return "observer";
    case uwb::protocol::ConnectionRole::Control:
        return "control";
    case uwb::protocol::ConnectionRole::None:
        return "none";
    }
    return "unknown";
}

std::string sessionName(uwb::protocol::SessionId session) {
    switch (session) {
    case uwb::protocol::SessionId::Default:
        return "default";
    case uwb::protocol::SessionId::Extended:
        return "extended";
    }
    return "unknown";
}

std::string capabilityList(uwb::protocol::CapabilityMask mask) {
    static constexpr Capability kOrder[] = {
        Capability::Range,
        Capability::PdoaAzimuth,
        Capability::SensorData,
        Capability::UwbConfigRead,
        Capability::UwbConfigWrite,
        Capability::TagManagement,
        Capability::RawAt,
        Capability::StreamLive,
        Capability::StreamRecording,
        Capability::MultiObserver,
    };

    std::string text;
    for (Capability capability : kOrder) {
        if (!uwb::protocol::hasCapability(mask, capability)) {
            continue;
        }
        if (!text.empty()) {
            text += ",";
        }
        text += uwb::protocol::capabilityName(capability);
    }
    if (text.empty()) {
        return "-";
    }
    return text;
}

const char *protocolErrorName(uwb::protocol::ProtocolErrorCode code) noexcept {
    using uwb::protocol::ProtocolErrorCode;
    switch (code) {
    case ProtocolErrorCode::None:
        return "None";
    case ProtocolErrorCode::NeedMoreData:
        return "NeedMoreData";
    case ProtocolErrorCode::HeaderTooShort:
        return "HeaderTooShort";
    case ProtocolErrorCode::InverseVersionMismatch:
        return "InverseVersionMismatch";
    case ProtocolErrorCode::UnsupportedMajorVersion:
        return "UnsupportedMajorVersion";
    case ProtocolErrorCode::UnknownPayloadType:
        return "UnknownPayloadType";
    case ProtocolErrorCode::PayloadTooLarge:
        return "PayloadTooLarge";
    case ProtocolErrorCode::InvalidPayloadLength:
        return "InvalidPayloadLength";
    case ProtocolErrorCode::TruncatedPayload:
        return "TruncatedPayload";
    case ProtocolErrorCode::UnexpectedPayloadLength:
        return "UnexpectedPayloadLength";
    case ProtocolErrorCode::InvalidField:
        return "InvalidField";
    case ProtocolErrorCode::InvalidReservedBits:
        return "InvalidReservedBits";
    case ProtocolErrorCode::DuplicateEntry:
        return "DuplicateEntry";
    case ProtocolErrorCode::EmptyServicePdu:
        return "EmptyServicePdu";
    case ProtocolErrorCode::UnknownServiceId:
        return "UnknownServiceId";
    case ProtocolErrorCode::UnsupportedService:
        return "UnsupportedService";
    case ProtocolErrorCode::UnsupportedSchemaVersion:
        return "UnsupportedSchemaVersion";
    case ProtocolErrorCode::InvalidChecksum:
        return "InvalidChecksum";
    }
    return "UnknownProtocolError";
}

} // namespace uwb::client
