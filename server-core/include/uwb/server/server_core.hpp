#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include "uwb/protocol/events.hpp"
#include "uwb/protocol/frame_parser.hpp"
#include "uwb/protocol/generic_header.hpp"
#include "uwb/server/clock.hpp"
#include "uwb/server/config_storage.hpp"
#include "uwb/server/connection_context.hpp"
#include "uwb/server/did_service.hpp"
#include "uwb/server/event_service.hpp"
#include "uwb/server/logger.hpp"
#include "uwb/server/routine_service.hpp"
#include "uwb/server/security_provider.hpp"
#include "uwb/server/server_config.hpp"
#include "uwb/server/service_dispatcher.hpp"
#include "uwb/server/transport.hpp"
#include "uwb/server/tx_queue.hpp"
#include "uwb/server/uwb_backend.hpp"

namespace uwb::server {

using uwb::protocol::FrameParser;

// Transport-independent server core facade (specification §43, §44;
// implementation plan §16).
//
// The host glue (simulator TCP loop, Pico lwIP RAW API glue) owns sockets and
// UARTs. The core sees only:
//   onConnect / onBytes / onDisconnect / tick
// and emits only complete encoded frames through IConnectionWriter.
class ServerCore {
public:
    struct Dependencies {
        ServerConfig config;
        IClock &clock;
        IConfigurationStorage &storage;
        ISecurityProvider *security = nullptr;
        IUwbBackend *backend = nullptr;
        ILogger *logger = nullptr;
    };

    // §24: the core queues the positive reset response; the host executes the
    // reset once the response has been flushed and the delay has elapsed.
    struct ResetRequest {
        bool hardReset = false;
        std::uint64_t executeAfterUs = 0;
    };

    explicit ServerCore(Dependencies &deps);

    // Static identity from hardware (board unique id -> UUID v5, specification §17).
    void configureIdentity(const std::array<std::uint8_t, 8> &boardUniqueId, const std::string &deviceName,
                           std::uint16_t logicalAddress, std::uint16_t tcpPort) noexcept;
    void setFirmwareVersion(uwb::protocol::ServerFirmwareVersionRecord version) noexcept;
    void setCapabilityMasks(uwb::protocol::CapabilityMask known, uwb::protocol::CapabilityMask detected) noexcept;

    // --- transport events ---------------------------------------------------
    void onConnect(ConnectionId connection, IConnectionWriter &writer, PeerInfo peer);
    void onBytes(ConnectionId connection, uwb::protocol::ConstBytes bytes);
    void onDisconnect(ConnectionId connection);

    // --- periodic work ------------------------------------------------------
    // Drives: backend UART timeouts, session/idle timeouts, alive checks, event
    // stream flush, TX scheduling, asynchronous response deadlines (§19, §22.3,
    // §38, §52, §53).
    void tick();

    // --- event producers (UWB pipeline, simulator, diagnostics) -------------
    std::size_t publishMeasurement(const uwb::protocol::MeasurementEvent &event);
    std::size_t publishLocalPosition(const uwb::protocol::LocalPositionEvent &event);
    std::size_t publishEvent(uwb::protocol::EventId event, uwb::protocol::ConstBytes payload);

    // --- host queries -------------------------------------------------------
    // False when the core has finished closing a connection: the host must close
    // the socket and then report onDisconnect().
    [[nodiscard]] bool wantsConnectionOpen(ConnectionId connection) const noexcept;

    // Reset the host must perform (§24). Consumed once.
    std::optional<ResetRequest> takeResetRequest();

    [[nodiscard]] bool backendReinitRequested() const noexcept { return backendReinitRequested_; }
    void clearBackendReinitRequest() noexcept { backendReinitRequested_ = false; }

    [[nodiscard]] DidService &dids() noexcept { return dids_; }
    [[nodiscard]] EventService &events() noexcept { return events_; }
    [[nodiscard]] RoutineService &routines() noexcept { return routines_; }
    [[nodiscard]] ConnectionRegistry &connections() noexcept { return connections_; }
    [[nodiscard]] const ConnectionRegistry &connections() const noexcept { return connections_; }
    [[nodiscard]] const ServerConfig &config() const noexcept { return deps_.config; }
    [[nodiscard]] std::uint64_t uptimeUs() const noexcept;

    [[nodiscard]] std::size_t connectionCount() const noexcept { return connections_.connectionCount(); }
    [[nodiscard]] std::size_t pendingRequestCount() const noexcept { return dispatcher_.pendingCount(); }

private:
    struct PerConnection {
        FrameParser parser;
    };

    void handleParserFailure(ConnectionId connection, PerConnection &state, ConnectionTxQueue &tx);
    void applyConnectionPolicies(std::uint64_t nowUs);
    void flushQueues();
    void publishConnectionStatus(std::uint64_t nowUs);
    void sendAliveCheck(ConnectionId connection, std::uint64_t nowUs);

    Dependencies &deps_;
    ConnectionRegistry connections_;
    ConnectionTxStore tx_;
    DidService dids_;
    RoutineService routines_;
    EventService events_;
    // Dependencies must be initialised before the dispatcher that references it.
    ServiceDispatcher::Dependencies dispatcherDeps_;
    ServiceDispatcher dispatcher_;

    std::unordered_map<ConnectionId, PerConnection> sessions_;
    std::unordered_map<ConnectionId, IConnectionWriter *> writers_;

    std::uint64_t bootUs_ = 0;
    bool configured_ = false;
    bool backendReinitRequested_ = false;
    std::optional<ResetRequest> resetRequest_;
};

} // namespace uwb::server
