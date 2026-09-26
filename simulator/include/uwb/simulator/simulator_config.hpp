#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "uwb/protocol/capabilities.hpp"
#include "uwb/protocol/constants.hpp"
#include "uwb/server/server_config.hpp"

namespace uwb::simulator {

using uwb::protocol::CapabilityMask;

// Rate at which the host transport drives SimulatorRuntime::tick(). The same
// constant is used by the stepped deterministic clock so logical time advances in
// fixed quanta (implementation plan §22.3).
inline constexpr std::uint32_t kSimulatorTickIntervalMs = 10;

// Human-use simulator configuration (implementation plan §22.4).
struct SimulatorOptions {
    // Identity (specification §6, §17).
    std::array<std::uint8_t, 8> boardUniqueId{1, 2, 3, 4, 5, 6, 7, 8};
    std::string deviceName = "sim-anchor";
    std::uint16_t logicalAddress = 0x1001;

    // Ports (specification §7). Port 0 means "let the OS choose" so integration
    // tests never collide with a real device.
    std::uint16_t udpPort = uwb::protocol::kDefaultProtocolPort;
    std::uint16_t tcpPort = uwb::protocol::kDefaultProtocolPort;
    std::string bindAddress = "0.0.0.0";

    // Capability reporting (specification §39, §41).
    CapabilityMask knownMask = uwb::protocol::kKnownCapabilityMask;
    CapabilityMask detectedMask = uwb::protocol::Capability::Range | uwb::protocol::Capability::PdoaAzimuth |
                                  uwb::protocol::Capability::SensorData | uwb::protocol::Capability::UwbConfigRead |
                                  uwb::protocol::Capability::UwbConfigWrite | uwb::protocol::Capability::TagManagement |
                                  uwb::protocol::Capability::RawAt | uwb::protocol::Capability::StreamLive |
                                  uwb::protocol::Capability::StreamRecording | uwb::protocol::Capability::MultiObserver;
    CapabilityMask overrideForceOn = 0;
    CapabilityMask overrideForceOff = 0;

    // Measurement simulation (specification §34, §35).
    std::uint32_t measurementPeriodMs = 100;
    std::uint32_t tagCount = 2;
    std::uint32_t anchorCount = 3;
    bool emitPdoa = true;
    bool emitLocalPosition = false;
    std::uint32_t rangeBaseMm = 1200;
    std::uint32_t rangeNoiseMm = 120;
    std::uint64_t randomSeed = 0x2545C4; // deterministic unless overridden

    // Fake backend behaviour (specification §48, §52, §53).
    std::uint32_t backendLatencyMs = 5;
    std::uint32_t backendInterCommandGapMs = 0;
    std::uint32_t backendCommandQueue = uwb::protocol::kUwbCommandQueueCapacity;

    // Fault injection for integration tests (plan §22.4). Configured at startup so a
    // test never has to mutate the backend while the simulator thread is running.
    bool backendRejectsSubmissions = false;
    bool backendFaultParseError = false;
    bool backendFaultTimeout = false;
    bool backendFaultUnsupported = false;

    // Server policy switches.
    bool securityEnabled = false;
    bool rawAtEnabled = true;
    std::uint32_t securityKey = 0x11223344U;
    std::uint16_t maxEventsPerSecond = 100;
    std::uint16_t maxStreamsPerConnection = 4;
    std::uint16_t p2ServerMaxMs = uwb::protocol::kDefaultP2ServerMaxMs;
    std::uint16_t p2StarServerMax10ms = uwb::protocol::kDefaultP2StarServerMax10ms;

    // Deterministic simulation (implementation plan §22.3).
    // With a stepped clock the core does not read wall-clock time: logical time
    // advances by exactly kSimulatorTickIntervalMs per simulator tick, so timeout
    // supervision, stream pacing and measurement scheduling are reproducible.
    bool deterministicClock = false;

    // Presentation.
    bool verbose = false;

    [[nodiscard]] uwb::server::ServerConfig serverConfig() const noexcept {
        uwb::server::ServerConfig config;
        config.securityEnabled = securityEnabled;
        config.rawAtEnabled = rawAtEnabled;
        config.maxEventsPerSecond = maxEventsPerSecond;
        config.maxStreamsPerConnection = maxStreamsPerConnection;
        config.p2ServerMaxMs = p2ServerMaxMs;
        config.p2StarServerMax10ms = p2StarServerMax10ms;
        config.queues.uwbCommandQueue = backendCommandQueue;
        return config;
    }

    [[nodiscard]] CapabilityMask effectiveMask() const noexcept {
        return uwb::protocol::applyCapabilityOverride(detectedMask, overrideForceOn, overrideForceOff);
    }
};

// Command-line parsing. Returns false and fills 'error' on a bad argument;
// 'usageRequested' is set for -h/--help.
bool parseSimulatorOptions(int argc, const char *const *argv, SimulatorOptions &out, std::string &error,
                           bool &usageRequested);

void printSimulatorUsage();
void printSimulatorOptions(const SimulatorOptions &options);

} // namespace uwb::simulator
