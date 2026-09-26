#include "uwb/server/server_core.hpp"

#include <vector>

#include "uwb/protocol/constants.hpp"
#include "uwb/protocol/generic_header.hpp"
#include "uwb/protocol/payload_types.hpp"
#include "uwb/protocol/uuid.hpp"

namespace uwb::server {

using uwb::protocol::ByteBuffer;
using uwb::protocol::ConstBytes;
using uwb::protocol::EventId;
using uwb::protocol::GenericHeaderNack;
using uwb::protocol::GenericHeaderNackCode;
using uwb::protocol::PayloadType;
using uwb::protocol::ProtocolErrorCode;

namespace {

// Maximum frames handed to the transport per connection per tick, so one fast
// peer cannot starve the others (specification §38).
inline constexpr std::size_t kMaxFramesPerConnectionPerTick = 16;

} // namespace

ServerCore::ServerCore(Dependencies &deps)
    : deps_(deps),
      connections_(deps.config.limits),
      tx_(deps.config.queues.connectionHighPriorityTx, deps.config.queues.connectionNormalPriorityTx),
      dids_(deps.config, deps.storage),
      routines_(),
      events_(deps.config, deps.clock),
      dispatcherDeps_{deps.config, deps.clock, connections_, dids_, routines_, events_, tx_, deps.security,
                      deps.backend},
      dispatcher_(dispatcherDeps_) {
    bootUs_ = deps_.clock.monotonicUs();
    dids_.setTimeouts(deps.config.timeouts);
    // §52: the core, not the backend, owns the connection-level timeouts.
    if (!validateServerConfig(deps.config).ok()) {
        if (deps_.logger != nullptr) {
            deps_.logger->error("ServerCore: invalid ServerConfig");
        }
    }
}

void ServerCore::configureIdentity(const std::array<std::uint8_t, 8> &boardUniqueId, const std::string &deviceName,
                                   std::uint16_t logicalAddress, std::uint16_t tcpPort) noexcept {
    const ConstBytes bytes{boardUniqueId.data(), boardUniqueId.size()};
    auto uuid = uwb::protocol::deviceUuidFromBoardId(bytes);
    Uuid deviceUuid =
        uuid.ok() ? uuid.value() : uwb::protocol::deviceUuidFromBoardIdU64(static_cast<std::uint64_t>(logicalAddress));
    dids_.setDeviceIdentity(deviceUuid, boardUniqueId, deviceName, logicalAddress, tcpPort);
    configured_ = true;
    backendReinitRequested_ = true;
}

void ServerCore::setFirmwareVersion(uwb::protocol::ServerFirmwareVersionRecord version) noexcept {
    dids_.setFirmwareVersion(version);
}

void ServerCore::setCapabilityMasks(uwb::protocol::CapabilityMask known, uwb::protocol::CapabilityMask detected) noexcept {
    dids_.setCapabilityMasks(known, detected);
}

// ---------------------------------------------------------------------------
// Transport events
// ---------------------------------------------------------------------------

void ServerCore::onConnect(ConnectionId connection, IConnectionWriter &writer, PeerInfo peer) {
    connections_.open(connection, peer, deps_.clock.monotonicUs());
    tx_.getOrCreate(connection); // §53: one bounded TX pair per connection
    writers_[connection] = &writer;
    sessions_.emplace(connection, PerConnection{FrameParser{uwb::protocol::kMaxProtocolPayload}});
}

void ServerCore::onBytes(ConnectionId connection, ConstBytes bytes) {
    auto session = sessions_.find(connection);
    if (session == sessions_.end()) {
        return;
    }
    ConnectionTxQueue *tx = tx_.find(connection);
    if (tx == nullptr) {
        return;
    }

    if (!session->second.parser.push(bytes).ok()) {
        handleParserFailure(connection, session->second, *tx);
        return;
    }

    while (session->second.parser.bufferedFrames() > 0) {
        auto frame = session->second.parser.popFrame();
        if (!frame.ok()) {
            handleParserFailure(connection, session->second, *tx);
            return;
        }
        dispatcher_.handleFrame(connection, frame.value());
    }
}

void ServerCore::onDisconnect(ConnectionId connection) {
    dispatcher_.connectionClosed(connection); // §9.4
    connections_.close(connection);
    tx_.remove(connection);
    sessions_.erase(connection);
    writers_.erase(connection);
}

// ---------------------------------------------------------------------------
// Periodic work
// ---------------------------------------------------------------------------

void ServerCore::tick() {
    const std::uint64_t nowUs = deps_.clock.monotonicUs();

    // Backend completions are delivered from the tick, never from another thread
    // inside the core (implementation plan §18, specification §48).
    if (deps_.backend != nullptr) {
        deps_.backend->tick(nowUs);
    }

    dids_.setUptime(nowUs - bootUs_);
    publishConnectionStatus(nowUs);
    applyConnectionPolicies(nowUs);

    for (auto &entry : sessions_) {
        ConnectionTxQueue *tx = tx_.find(entry.first);
        if (tx != nullptr) {
            std::ignore = events_.flush(entry.first, *tx, nowUs);
        }
    }

    dispatcher_.tick(nowUs);
    flushQueues();

    const ServiceDispatcher::HostAction action = dispatcher_.takeHostAction();
    if (action.resetPico) {
        resetRequest_ = ResetRequest{action.hardReset, action.resetAfterUs};
    }
    if (action.reinitBackend) {
        backendReinitRequested_ = true;
    }

    // A Closing connection is finished once everything it must send has been sent.
    std::vector<ConnectionId> finished;
    for (const auto &ctx : connections_.connections()) {
        if (ctx.state != ConnectionState::Closing) {
            continue;
        }
        const ConnectionTxQueue *tx = tx_.find(ctx.id);
        if (tx == nullptr || tx->empty()) {
            finished.push_back(ctx.id);
        }
    }
    for (ConnectionId id : finished) {
        connections_.markClosed(id);
    }
}

void ServerCore::applyConnectionPolicies(std::uint64_t nowUs) {
    std::vector<ConnectionId> ids;
    ids.reserve(connections_.connectionCount());
    for (const auto &ctx : connections_.connections()) {
        ids.push_back(ctx.id);
    }

    const std::uint64_t sessionTimeoutUs = msToUs(deps_.config.timeouts.sessionInactivityTimeoutMs);
    const std::uint64_t idleTimeoutUs = msToUs(deps_.config.timeouts.tcpIdleTimeoutMs);
    const std::uint64_t aliveIntervalUs = msToUs(deps_.config.timeouts.aliveCheckIntervalMs);

    for (ConnectionId id : ids) {
        const ConnectionContext *ctx = connections_.find(id);
        if (ctx == nullptr) {
            continue;
        }
        const std::uint64_t idleUs = nowUs - ctx->lastActivityUs;

        // §22.3: an inactive Extended Session returns to Default Session and
        // clears the security-unlocked state.
        if (ctx->isExtendedSession() && idleUs > sessionTimeoutUs) {
            connections_.enterDefaultSession(id);
            if (deps_.security != nullptr) {
                deps_.security->reset(id);
            }
            if (deps_.logger != nullptr) {
                deps_.logger->warn("session inactivity timeout: Extended -> Default");
            }
        }

        // §19: keepalive is only needed while the peer is otherwise silent.
        if (idleUs >= aliveIntervalUs && nowUs - ctx->lastAliveCheckUs >= aliveIntervalUs) {
            sendAliveCheck(id, nowUs);
        }

        // §52: a completely silent connection is closed. This also covers a TCP
        // connection that never sends a valid Connection Activation Request (§9.1).
        if (idleUs > idleTimeoutUs) {
            connections_.markClosing(id);
        }
    }
}

void ServerCore::sendAliveCheck(ConnectionId connection, std::uint64_t nowUs) {
    ConnectionTxQueue *tx = tx_.find(connection);
    if (tx == nullptr) {
        return;
    }
    const ByteBuffer payload =
        uwb::protocol::encodeAliveCheckRequest(uwb::protocol::AliveCheckRequest{nowUs});
    auto frame = uwb::protocol::encodeFrame(PayloadType::AliveCheckRequest, uwb::protocol::bytesOf(payload));
    if (!frame.ok()) {
        return;
    }
    std::ignore = tx->push(uwb::protocol::bytesOf(frame.value()), TxPriority::High);
    if (ConnectionContext *ctx = connections_.find(connection); ctx != nullptr) {
        ctx->lastAliveCheckUs = nowUs;
    }
}

void ServerCore::flushQueues() {
    for (auto &entry : sessions_) {
        const ConnectionId id = entry.first;
        ConnectionTxQueue *tx = tx_.find(id);
        const auto writerIt = writers_.find(id);
        IConnectionWriter *writer = writerIt == writers_.end() ? nullptr : writerIt->second;
        if (tx == nullptr || writer == nullptr) {
            continue;
        }

        if (!writer->isOpen()) {
            connections_.markClosing(id);
            continue;
        }

        std::size_t sent = 0;
        while (sent < kMaxFramesPerConnectionPerTick) {
            const ByteBuffer *frame = tx->peekNext();
            if (frame == nullptr) {
                break;
            }
            // Non-blocking transport: a peer that cannot take the frame now keeps
            // it queued; the core does not drop control traffic (§38).
            if (!writer->writeFrame(uwb::protocol::bytesOf(*frame))) {
                break;
            }
            tx->popFront();
            ++sent;
        }
    }
}

void ServerCore::publishConnectionStatus(std::uint64_t nowUs) {
    uwb::protocol::ConnectionStatusRecord status;
    status.controlOccupied = connections_.controlCount() > 0 ? 1 : 0;
    status.activeObservers = static_cast<std::uint8_t>(connections_.observerCount());
    status.maxObservers = static_cast<std::uint8_t>(deps_.config.limits.maxObserverConnections);
    status.controlClientLogicalAddress = 0;
    status.activeSession = 0;
    for (const auto &ctx : connections_.connections()) {
        if (ctx.isControl()) {
            status.controlClientLogicalAddress = ctx.clientLogicalAddress;
            status.activeSession = static_cast<std::uint8_t>(toSessionId(ctx.session));
            break;
        }
    }
    (void)nowUs;
    dids_.setConnectionStatus(status);
}

void ServerCore::handleParserFailure(ConnectionId connection, PerConnection &state, ConnectionTxQueue &tx) {
    const auto error = state.parser.error();
    const auto mapped = uwb::protocol::genericHeaderNackFor(error.has_value() ? error->code : ProtocolErrorCode::None);
    const auto code = mapped.has_value() ? *mapped : GenericHeaderNackCode::IncorrectHeaderPattern;
    const ByteBuffer payload = uwb::protocol::encodeGenericHeaderNack(GenericHeaderNack{code});
    auto frame = uwb::protocol::encodeFrame(PayloadType::GenericHeaderNack, uwb::protocol::bytesOf(payload));
    if (frame.ok()) {
        std::ignore = tx.push(uwb::protocol::bytesOf(frame.value()), TxPriority::High);
    }
    if (deps_.logger != nullptr) {
        deps_.logger->warn("frame header violation: closing connection");
    }
    connections_.markClosing(connection); // §13: header violations are connection-fatal
}

// ---------------------------------------------------------------------------
// Event producers
// ---------------------------------------------------------------------------

std::size_t ServerCore::publishMeasurement(const uwb::protocol::MeasurementEvent &event) {
    const ByteBuffer payload = uwb::protocol::encodeMeasurementEvent(event);
    return events_.publish(EventId::UwbMeasurement, uwb::protocol::bytesOf(payload));
}

std::size_t ServerCore::publishLocalPosition(const uwb::protocol::LocalPositionEvent &event) {
    const ByteBuffer payload = uwb::protocol::encodeLocalPositionEvent(event);
    return events_.publish(EventId::UwbLocalPosition, uwb::protocol::bytesOf(payload));
}

std::size_t ServerCore::publishEvent(EventId event, ConstBytes payload) {
    return events_.publish(event, payload);
}

// ---------------------------------------------------------------------------
// Host queries
// ---------------------------------------------------------------------------

bool ServerCore::wantsConnectionOpen(ConnectionId connection) const noexcept {
    const ConnectionContext *ctx = connections_.find(connection);
    return ctx != nullptr && ctx->state != ConnectionState::Closed;
}

std::optional<ServerCore::ResetRequest> ServerCore::takeResetRequest() {
    if (!resetRequest_.has_value()) {
        return std::nullopt;
    }
    ResetRequest request = resetRequest_.value();
    resetRequest_.reset();
    return request;
}

std::uint64_t ServerCore::uptimeUs() const noexcept {
    return deps_.clock.monotonicUs() - bootUs_;
}

} // namespace uwb::server
