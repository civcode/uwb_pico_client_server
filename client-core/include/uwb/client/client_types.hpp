#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "uwb/domain/device_identity.hpp"
#include "uwb/protocol/capabilities.hpp"
#include "uwb/protocol/constants.hpp"
#include "uwb/protocol/dids.hpp"
#include "uwb/protocol/errors.hpp"
#include "uwb/protocol/payloads.hpp"
#include "uwb/protocol/services.hpp"

namespace uwb::client {

// Network endpoint of a device (host textual form + TCP port).
struct Endpoint {
    std::string host;
    std::uint16_t port = uwb::protocol::kDefaultProtocolPort;

    [[nodiscard]] std::string toString() const;
    [[nodiscard]] bool operator==(const Endpoint &other) const noexcept {
        return host == other.host && port == other.port;
    }
    [[nodiscard]] bool operator!=(const Endpoint &other) const noexcept { return !(*this == other); }
};

[[nodiscard]] std::optional<Endpoint> parseEndpoint(std::string_view text) noexcept;

// Client connection state machine (specification §91).
enum class ConnectionState : std::uint8_t {
    Disconnected = 0,
    Connecting = 1,
    TcpConnected = 2,
    Activated = 3,
    Ready = 4,
    ReconnectWait = 5,
    Closing = 6,
};

[[nodiscard]] const char *connectionStateName(ConnectionState state) noexcept;

// Client timeout defaults (specification §73).
struct ClientTimeouts {
    std::uint32_t discoveryWindowMs = 500;
    std::uint32_t connectTimeoutMs = 3000;
    std::uint32_t requestTimeoutMs = 2000;
    std::uint32_t longRoutineTimeoutMs = 10000;
    std::uint32_t reconnectInitialDelayMs = 500;
    std::uint32_t reconnectMaxDelayMs = 10000;
    std::uint32_t aliveCheckIdleMs = 10000;      // §19: send after the connection was idle
    std::uint32_t clientPresentIntervalMs = 2000; // §29: extended-session maintenance
};

struct ClientOptions {
    // Client instance UUID shared by CLI/TUI/GUI of one installation (§18.1).
    // A zero UUID is valid for anonymous/test clients.
    domain::DeviceUuid clientInstanceUuid;

    // Client logical addresses are allocated from the client range in
    // specification §39/§9 so that several connections of one process stay
    // distinguishable per device.
    std::uint16_t clientLogicalAddressBase = uwb::protocol::kClientLogicalAddressMin + 1;

    // Role requested during activation. The server may downgrade it to Observer
    // when the control slot is taken (§18.2).
    uwb::protocol::ConnectionRole desiredRole = uwb::protocol::ConnectionRole::Control;

    ClientTimeouts timeouts;
    bool autoReconnect = true;
    std::uint32_t maxOutstandingRequests = 16;
    std::uint32_t maxTxQueueFrames = 32;

    // Random jitter added to the reconnect backoff by the networking layer.
    // Zero (the default) keeps unit tests exact.
    std::function<std::uint32_t(std::uint32_t)> reconnectJitter;
};

// Compact, UI-friendly view of one discovered/connected device.
struct DeviceSummary {
    domain::DeviceUuid uuid;
    std::string name;
    Endpoint endpoint;
    std::uint16_t logicalAddress = uwb::protocol::kLogicalAddressInvalid;

    ConnectionState state = ConnectionState::Disconnected;
    uwb::protocol::ConnectionRole role = uwb::protocol::ConnectionRole::None;
    uwb::protocol::SessionId session = uwb::protocol::SessionId::Default;

    uwb::protocol::CapabilityMask capabilities = 0;
    bool controlOccupied = false;
    std::uint8_t activeObservers = 0;
    std::uint8_t maxObservers = 0;

    std::uint32_t subscriptions = 0;
    std::uint64_t lastSeenUs = 0;
    bool hasError = false;
    std::string lastError;
};

[[nodiscard]] std::string roleName(uwb::protocol::ConnectionRole role);
[[nodiscard]] std::string sessionName(uwb::protocol::SessionId session);
[[nodiscard]] std::string capabilityList(uwb::protocol::CapabilityMask mask);
[[nodiscard]] const char *protocolErrorName(uwb::protocol::ProtocolErrorCode code) noexcept;

} // namespace uwb::client
