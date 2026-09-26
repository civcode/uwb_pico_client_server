#pragma once

#include <cstdint>

#include "uwb/protocol/protocol_version.hpp"
#include "uwb/protocol/service_ids.hpp"

namespace uwb::server {

using uwb::protocol::ConnectionRole;
using uwb::protocol::SessionId;

// Lifecycle of one TCP connection inside the server core (specification §9).
enum class ConnectionState : std::uint8_t {
    // TCP accepted, no valid Connection Activation frame yet.
    AwaitingActivation = 0,
    // Activated: role assigned, application services allowed.
    Active = 1,
    // A response that must be flushed before closing has been queued (§9.4).
    Closing = 2,
    // No longer usable; the host transport must close the socket.
    Closed = 3,
};

// Session state (specification §22).
enum class SessionState : std::uint8_t {
    Default = 0,
    Extended = 1,
};

inline constexpr SessionId toSessionId(SessionState state) noexcept {
    return state == SessionState::Extended ? SessionId::Extended : SessionId::Default;
}

// Security state, orthogonal to session state (specification §22.2).
// Extended Session can be entered without security when the device is not
// security protected.
enum class SecurityState : std::uint8_t {
    Locked = 0,
    SeedIssued = 1,
    Unlocked = 2,
};

[[nodiscard]] constexpr bool isKnownConnectionState(std::uint8_t value) noexcept {
    return value <= 3;
}

// Authorization levels used by the dispatcher (specification §9.2).
enum class AccessLevel : std::uint8_t {
    // Allowed for Control and Observer.
    AnyActiveConnection = 0,
    // Allowed for Control connections only.
    ControlOnly = 1,
    // Requires Extended Session in addition to the role requirement (§22.3).
    ControlExtended = 2,
    // Only a connection that already passed Security Access (§25).
    SecurityUnlocked = 3,
};

} // namespace uwb::server
