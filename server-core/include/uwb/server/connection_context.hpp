#pragma once

#include <cstdint>
#include <vector>

#include "uwb/protocol/bytes.hpp"
#include "uwb/protocol/service_ids.hpp"
#include "uwb/server/connection_state.hpp"
#include "uwb/server/security_provider.hpp"
#include "uwb/server/server_config.hpp"
#include "uwb/server/transport.hpp"

namespace uwb::server {

using uwb::protocol::ActivationResponseCode;
using uwb::protocol::ByteBuffer;

// Per-connection state owned by the server core (specification §43 Connection
// Manager). Nothing here knows about sockets or UARTs.
struct ConnectionContext {
    ConnectionId id = kInvalidConnectionId;
    ConnectionState state = ConnectionState::AwaitingActivation;
    ConnectionRole role = ConnectionRole::None;
    ConnectionRole requestedRole = ConnectionRole::None;
    ActivationResponseCode activationCode = ActivationResponseCode::AcceptedRequestedRole;

    // Negotiated generic-header version byte (§12), e.g. 0x10.
    std::uint8_t protocolVersionByte = uwb::protocol::kProtocolVersionV1_0;
    SessionState session = SessionState::Default;
    SecurityState security = SecurityState::Locked;
    PeerInfo peer{};

    ByteBuffer deviceUuid;          // 16 bytes echoed from discovery
    ByteBuffer clientInstanceUuid;  // 16 bytes
    std::uint16_t clientLogicalAddress = 0;

    std::uint64_t openedAtUs = 0;
    std::uint64_t lastActivityUs = 0;
    std::uint64_t lastAliveCheckUs = 0;

    // Security bookkeeping (specification §25, §22.3).
    SecuritySeed seed = 0;
    std::uint64_t seedIssuedUs = 0;
    std::uint32_t failedAttempts = 0;
    std::uint64_t blockedUntilUs = 0;

    // Last request SID acknowledged with an Application Ack/Nack (§20.3).
    std::uint8_t lastRequestSid = 0;

    [[nodiscard]] bool isActive() const noexcept { return state == ConnectionState::Active; }
    [[nodiscard]] bool isControl() const noexcept { return role == ConnectionRole::Control; }
    [[nodiscard]] bool isObserver() const noexcept { return role == ConnectionRole::Observer; }
    [[nodiscard]] bool isExtendedSession() const noexcept { return session == SessionState::Extended; }
    [[nodiscard]] bool isUnlocked() const noexcept { return security == SecurityState::Unlocked; }
};

// Result of the role/slot policy (specification §9.1..§9.3).
struct RoleDecision {
    bool accepted = false;
    ConnectionRole assigned = ConnectionRole::None;
    ActivationResponseCode code = ActivationResponseCode::RejectedNoConnectionSlot;
};

// Owns the connection slots and enforces §9.1..§9.4:
//   * one Control slot, a configurable number of Observer slots, a total TCP cap
//   * observers may be downgraded, never silently promoted
//   * a raw connection that never activates gets no slots
class ConnectionRegistry {
public:
    explicit ConnectionRegistry(const ConnectionLimits &limits) noexcept;

    // Called when the transport accepts a TCP connection. The context starts in
    // AwaitingActivation and holds no role.
    ConnectionContext &open(ConnectionId id, PeerInfo peer, std::uint64_t nowUs);

    // Called when the transport reports a disconnect. Idempotent.
    void close(ConnectionId id);

    [[nodiscard]] ConnectionContext *find(ConnectionId id) noexcept;
    [[nodiscard]] const ConnectionContext *find(ConnectionId id) const noexcept;

    [[nodiscard]] std::size_t connectionCount() const noexcept { return connections_.size(); }
    [[nodiscard]] std::size_t controlCount() const noexcept;
    [[nodiscard]] std::size_t observerCount() const noexcept;

    // Role assignment for an Activation Request (specification §9.1..§9.3).
    // Observers requesting Control are answered with
    // RejectedRolePolicyDenied: v1 does not auto-promote observers and does not
    // preempt the current Control holder.
    RoleDecision assignRole(ConnectionId id, ConnectionRole requested, std::uint64_t nowUs);

    // Explicit Control release (Routine 0x0200) so another client can take the
    // slot on the same or a new connection (specification §9.3).
    bool releaseControl(ConnectionId id) noexcept;

    // Whether an observer that lost its slot may re-activate as Observer after a
    // slot frees (§9.3).
    bool hasObserverSlot() const noexcept;

    void touch(ConnectionId id, std::uint64_t nowUs) noexcept;
    void enterExtendedSession(ConnectionId id, std::uint64_t nowUs) noexcept;
    void enterDefaultSession(ConnectionId id) noexcept;

    // Move a connection to Closing (flush responses, then close) or Closed.
    void markClosing(ConnectionId id) noexcept;
    void markClosed(ConnectionId id) noexcept;

    [[nodiscard]] const std::vector<ConnectionContext> &connections() const noexcept { return connections_; }

private:
    [[nodiscard]] std::size_t totalCapacity() const noexcept;

    ConnectionLimits limits_;
    std::vector<ConnectionContext> connections_;
};

} // namespace uwb::server
