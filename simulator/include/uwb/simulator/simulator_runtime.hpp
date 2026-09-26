#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

#include "uwb/protocol/payloads.hpp"
#include "uwb/protocol/uuid.hpp"
#include "uwb/server/server_core.hpp"
#include "uwb/server/transport.hpp"
#include "uwb/simulator/host_services.hpp"
#include "uwb/simulator/simulated_backend.hpp"
#include "uwb/simulator/simulator_config.hpp"

namespace uwb::simulator {

using uwb::protocol::ByteBuffer;
using uwb::protocol::ConstBytes;
using uwb::server::ConnectionId;
using uwb::server::IConnectionWriter;
using uwb::server::PeerInfo;

// Host simulator runtime (implementation plan §20, §22).
//
// It wires ServerCore together with host implementations of IClock,
// IConfigurationStorage, ISecurityProvider and IUwbBackend. It contains no socket
// code at all, so the same runtime drives the CLI simulator and the integration
// tests, and the Phase 6 firmware can mirror this wiring on the Pico.
class SimulatorRuntime {
public:
    explicit SimulatorRuntime(const SimulatorOptions &options);

    // --- transport facing API -----------------------------------------------
    void onConnect(ConnectionId connection, IConnectionWriter &writer, const PeerInfo &peer);
    void onBytes(ConnectionId connection, ConstBytes data);
    void onDisconnect(ConnectionId connection);

    // True while the core still wants the socket open (specification §18.4).
    [[nodiscard]] bool wantsConnectionOpen(ConnectionId connection) const;

    // Advance the core, the fake backend, and the measurement source. Call at a
    // regular interval (the Asio transport uses a 10 ms timer).
    void tick();

    // --- discovery (§8) ------------------------------------------------------
    // Builds the 0x0004 Device Identification Response payload for the current
    // runtime state; returns nullopt for malformed requests.
    // Non-const because the device identity is read through DidService, which the
    // core deliberately does not expose as a const accessor.
    [[nodiscard]] std::optional<ByteBuffer>
    discoveryResponse(ConstBytes datagram, const uwb::protocol::Uuid &deviceUuid);

    [[nodiscard]] uwb::protocol::DeviceIdResponse identityResponse();

    // --- accessors -----------------------------------------------------------
    [[nodiscard]] uwb::server::ServerCore &core() noexcept { return core_; }
    [[nodiscard]] const uwb::server::ServerCore &core() const noexcept { return core_; }
    [[nodiscard]] SimulatedUwbBackend &backend() noexcept { return backend_; }
    [[nodiscard]] MemoryConfigStorage &storage() noexcept { return storage_; }
    [[nodiscard]] uwb::server::IClock &clock() noexcept { return clock_; }
    [[nodiscard]] std::uint64_t nowUs() const noexcept { return clock_.monotonicUs(); }
    [[nodiscard]] const SimulatorOptions &options() const noexcept { return options_; }

    // Observation counters for tests and the CLI. The core itself is single
    // threaded, so these snapshots are published by the runtime thread and read
    // atomically instead of reaching into ServerCore from another thread.
    [[nodiscard]] std::size_t measurementEventsPublished() const noexcept {
        return measurementsPublished_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::size_t simulatedResets() const noexcept { return resets_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::size_t connectionCount() const noexcept {
        return connections_.load(std::memory_order_relaxed);
    }

    // Environment knobs used by tests and the CLI.
    void setWifiRssi(std::int16_t rssiDbm);

    // Non-null only in deterministic mode; lets a test drive logical time directly.
    [[nodiscard]] ManualClock *manualClock() noexcept {
        return options_.deterministicClock ? &manualClock_ : nullptr;
    }

private:
    void publishMeasurements(std::uint64_t nowUs);
    void handleHostActions(std::uint64_t nowUs);
    void publishConnectionCount() noexcept;

    SimulatorOptions options_;
    SystemClock systemClock_;
    ManualClock manualClock_;
    // Bound to the manual clock when options.deterministicClock is set (plan §22.3),
    // otherwise to the wall clock. Declared after both concrete clocks.
    uwb::server::IClock &clock_;
    MemoryConfigStorage storage_;
    StaticSecurityProvider security_;
    ConsoleLogger logger_;
    SimulatedUwbBackend backend_;
    uwb::server::ServerCore::Dependencies deps_;
    uwb::server::ServerCore core_;

    std::uint64_t lastLocalPositionUs_ = 0;
    std::atomic<std::size_t> measurementsPublished_{0};
    std::atomic<std::size_t> resets_{0};
    std::atomic<std::size_t> connections_{0};
};

} // namespace uwb::simulator
