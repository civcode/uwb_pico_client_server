#include "uwb/server/connection_context.hpp"

#include <algorithm>

namespace uwb::server {

ConnectionRegistry::ConnectionRegistry(const ConnectionLimits &limits) noexcept : limits_(limits) {}

std::size_t ConnectionRegistry::totalCapacity() const noexcept {
    return limits_.maxTcpConnections;
}

ConnectionContext &ConnectionRegistry::open(ConnectionId id, PeerInfo peer, std::uint64_t nowUs) {
    for (auto &ctx : connections_) {
        if (ctx.id == id) {
            ctx.lastActivityUs = nowUs;
            return ctx;
        }
    }
    ConnectionContext ctx;
    ctx.id = id;
    ctx.state = ConnectionState::AwaitingActivation;
    ctx.peer = peer;
    ctx.openedAtUs = nowUs;
    ctx.lastActivityUs = nowUs;
    ctx.lastAliveCheckUs = nowUs;
    connections_.push_back(ctx);
    return connections_.back();
}

void ConnectionRegistry::close(ConnectionId id) {
    connections_.erase(std::remove_if(connections_.begin(), connections_.end(),
                                      [id](const ConnectionContext &ctx) { return ctx.id == id; }),
                       connections_.end());
}

ConnectionContext *ConnectionRegistry::find(ConnectionId id) noexcept {
    auto it = std::find_if(connections_.begin(), connections_.end(),
                           [id](const ConnectionContext &ctx) { return ctx.id == id; });
    return it == connections_.end() ? nullptr : &*it;
}

const ConnectionContext *ConnectionRegistry::find(ConnectionId id) const noexcept {
    auto it = std::find_if(connections_.begin(), connections_.end(),
                           [id](const ConnectionContext &ctx) { return ctx.id == id; });
    return it == connections_.end() ? nullptr : &*it;
}

std::size_t ConnectionRegistry::controlCount() const noexcept {
    return static_cast<std::size_t>(std::count_if(connections_.begin(), connections_.end(), [](const ConnectionContext &ctx) {
        return ctx.isActive() && ctx.isControl();
    }));
}

std::size_t ConnectionRegistry::observerCount() const noexcept {
    return static_cast<std::size_t>(std::count_if(connections_.begin(), connections_.end(), [](const ConnectionContext &ctx) {
        return ctx.isActive() && ctx.isObserver();
    }));
}

RoleDecision ConnectionRegistry::assignRole(ConnectionId id, ConnectionRole requested, std::uint64_t nowUs) {
    RoleDecision decision;
    auto *ctx = find(id);
    if (ctx == nullptr) {
        decision.code = ActivationResponseCode::RejectedServerNotReady;
        return decision;
    }

    auto accept = [&](ConnectionRole role, ActivationResponseCode code) {
        ctx->role = role;
        ctx->state = ConnectionState::Active;
        ctx->requestedRole = requested;
        ctx->activationCode = code;
        ctx->lastActivityUs = nowUs;
        decision.accepted = true;
        decision.assigned = role;
        decision.code = code;
    };

    const bool totalCapacityFree = connections_.size() <= limits_.maxTcpConnections;
    const bool controlFree = controlCount() < limits_.maxControlConnections;
    const bool observerFree = observerCount() < limits_.maxObserverConnections;

    // Re-activation of an already active connection (§9.3): an Observer may ask
    // for Control once the slot is free; roles are never promoted implicitly.
    if (ctx->isActive()) {
        if (requested == ctx->role) {
            accept(ctx->role, ActivationResponseCode::AcceptedRequestedRole);
            return decision;
        }
        if (requested == ConnectionRole::Control && ctx->isObserver() && controlFree) {
            accept(ConnectionRole::Control, ActivationResponseCode::AcceptedRequestedRole);
            return decision;
        }
        decision.code = ActivationResponseCode::RejectedRolePolicyDenied;
        return decision;
    }

    if (!totalCapacityFree) {
        decision.code = ActivationResponseCode::RejectedNoConnectionSlot;
        return decision;
    }

    if (requested == ConnectionRole::Control) {
        if (controlFree) {
            accept(ConnectionRole::Control, ActivationResponseCode::AcceptedRequestedRole); // §18.2 code 0x00
            return decision;
        }
        // Control occupied: downgrade to Observer when an Observer slot exists,
        // otherwise reject and close (§9.3).
        if (observerFree) {
            accept(ConnectionRole::Observer, ActivationResponseCode::AcceptedDowngradedToObserver); // §18.2 code 0x01
            return decision;
        }
        decision.code = ActivationResponseCode::RejectedNoConnectionSlot;
        return decision;
    }

    if (requested == ConnectionRole::Observer) {
        if (observerFree) {
            accept(ConnectionRole::Observer, ActivationResponseCode::AcceptedRequestedRole);
            return decision;
        }
        decision.code = ActivationResponseCode::RejectedNoConnectionSlot;
        return decision;
    }

    decision.code = ActivationResponseCode::RejectedRolePolicyDenied;
    return decision;
}

bool ConnectionRegistry::releaseControl(ConnectionId id) noexcept {
    auto *ctx = find(id);
    if (ctx == nullptr || !ctx->isControl()) {
        return false;
    }
    ctx->role = ConnectionRole::None;
    ctx->state = ConnectionState::Closing;
    ctx->session = SessionState::Default;
    ctx->security = SecurityState::Locked;
    return true;
}

bool ConnectionRegistry::hasObserverSlot() const noexcept {
    return observerCount() < limits_.maxObserverConnections && connections_.size() < limits_.maxTcpConnections;
}

void ConnectionRegistry::touch(ConnectionId id, std::uint64_t nowUs) noexcept {
    auto *ctx = find(id);
    if (ctx != nullptr) {
        ctx->lastActivityUs = nowUs;
    }
}

void ConnectionRegistry::enterExtendedSession(ConnectionId id, std::uint64_t nowUs) noexcept {
    auto *ctx = find(id);
    if (ctx == nullptr) {
        return;
    }
    ctx->session = SessionState::Extended;
    ctx->lastActivityUs = nowUs;
}

void ConnectionRegistry::enterDefaultSession(ConnectionId id) noexcept {
    auto *ctx = find(id);
    if (ctx == nullptr) {
        return;
    }
    ctx->session = SessionState::Default;
    ctx->security = SecurityState::Locked;
    ctx->seed = 0;
    ctx->seedIssuedUs = 0;
}

void ConnectionRegistry::markClosing(ConnectionId id) noexcept {
    auto *ctx = find(id);
    if (ctx != nullptr && ctx->state != ConnectionState::Closed) {
        ctx->state = ConnectionState::Closing;
    }
}

void ConnectionRegistry::markClosed(ConnectionId id) noexcept {
    auto *ctx = find(id);
    if (ctx != nullptr) {
        ctx->state = ConnectionState::Closed;
    }
}

} // namespace uwb::server
