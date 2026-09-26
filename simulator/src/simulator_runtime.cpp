#include "uwb/simulator/simulator_runtime.hpp"

#include <array>
#include <string>

#include "uwb/protocol/constants.hpp"
#include "uwb/protocol/generic_header.hpp"
#include "uwb/protocol/payload_types.hpp"
#include "uwb/server/server_errors.hpp"

namespace uwb::simulator {
namespace {

using uwb::protocol::ControlStatus;
using uwb::protocol::DeviceIdRequest;
using uwb::protocol::DeviceIdResponse;
using uwb::protocol::FrameParser;
using uwb::protocol::PayloadType;
using uwb::protocol::Uuid;

constexpr uwb::protocol::ServerFirmwareVersionRecord kSimulatorFirmware = {0, 1, 0, 1};

// Chooses the logical clock for the runtime (plan §22.3).
uwb::server::IClock &selectClock(const SimulatorOptions &options, SystemClock &systemClock,
                                ManualClock &manualClock) noexcept {
    return options.deterministicClock ? static_cast<uwb::server::IClock &>(manualClock)
                                      : static_cast<uwb::server::IClock &>(systemClock);
}

} // namespace

SimulatorRuntime::SimulatorRuntime(const SimulatorOptions &options)
    : options_(options), clock_(selectClock(options, systemClock_, manualClock_)),
      security_(options.securityKey), logger_(options.verbose),
      backend_(SimulatedUwbBackend::Settings{options.backendLatencyMs, options.backendInterCommandGapMs,
                                             uwb::protocol::kDefaultUartCommandTimeoutMs, options.backendCommandQueue},
               options),
      deps_(uwb::server::ServerCore::Dependencies{options.serverConfig(), clock_, storage_, &security_, &backend_,
                                                  &logger_}),
      core_(deps_) {
    core_.configureIdentity(options_.boardUniqueId, options_.deviceName, options_.logicalAddress, options_.tcpPort);
    core_.setFirmwareVersion(kSimulatorFirmware);
    core_.setCapabilityMasks(options_.knownMask, options_.detectedMask);
    core_.dids().setCapabilityOverride(
        uwb::protocol::CapabilityOverrideRecord{options_.overrideForceOn, options_.overrideForceOff});
    core_.clearBackendReinitRequest();
    storage_.seed(ByteBuffer{});
}

// ---------------------------------------------------------------------------
// Transport facing API (identical shape to the future Pico lwIP glue)
// ---------------------------------------------------------------------------
void SimulatorRuntime::onConnect(ConnectionId connection, IConnectionWriter &writer, const PeerInfo &peer) {
    core_.onConnect(connection, writer, peer);
    publishConnectionCount();
}

void SimulatorRuntime::onBytes(ConnectionId connection, ConstBytes data) { core_.onBytes(connection, data); }

void SimulatorRuntime::onDisconnect(ConnectionId connection) {
    core_.onDisconnect(connection);
    publishConnectionCount();
}

bool SimulatorRuntime::wantsConnectionOpen(ConnectionId connection) const {
    return core_.wantsConnectionOpen(connection);
}

void SimulatorRuntime::tick() {
    if (options_.deterministicClock) {
        // Logical time advances in fixed quanta instead of wall-clock time so
        // timeout supervision and stream pacing are reproducible (plan §22.3).
        manualClock_.advanceMs(kSimulatorTickIntervalMs);
    }
    const std::uint64_t nowUs = clock_.monotonicUs();
    publishMeasurements(nowUs);
    core_.tick();
    handleHostActions(nowUs);
    publishConnectionCount();
}

void SimulatorRuntime::publishConnectionCount() noexcept {
    connections_.store(core_.connectionCount(), std::memory_order_relaxed);
}

void SimulatorRuntime::publishMeasurements(std::uint64_t nowUs) {
    if (!backend_.measurementDue(nowUs)) {
        return;
    }
    const uwb::protocol::MeasurementEvent measurement = backend_.nextMeasurement(nowUs);
    measurementsPublished_ += core_.publishMeasurement(measurement);

    if (options_.emitLocalPosition) {
        const std::uint64_t periodUs = static_cast<std::uint64_t>(options_.measurementPeriodMs) * 2000ULL;
        if (lastLocalPositionUs_ == 0 || nowUs >= lastLocalPositionUs_ + periodUs) {
            lastLocalPositionUs_ = nowUs;
            core_.publishLocalPosition(backend_.nextLocalPosition(nowUs));
        }
    }
}

void SimulatorRuntime::handleHostActions(std::uint64_t) {
    if (const auto reset = core_.takeResetRequest(); reset.has_value()) {
        ++resets_;
        logger_.info(reset->hardReset ? "simulated hard device reset" : "simulated soft device reset");
    }
    if (core_.backendReinitRequested()) {
        core_.clearBackendReinitRequest();
        logger_.debug("simulated UWB backend reinitialisation");
    }
}

void SimulatorRuntime::setWifiRssi(std::int16_t rssiDbm) { core_.dids().setWifiRssi(rssiDbm); }

// ---------------------------------------------------------------------------
// Discovery (§8): one frame per UDP datagram
// ---------------------------------------------------------------------------
uwb::protocol::DeviceIdResponse SimulatorRuntime::identityResponse() {
    uwb::protocol::DeviceIdResponse response;
    response.deviceUuid = core_.dids().deviceUuid();
    response.logicalAddress = core_.dids().logicalAddress();
    response.tcpPort = core_.dids().tcpPort();
    response.capabilityMask = core_.dids().effectiveCapabilities();
    response.controlStatus = core_.connections().controlCount() != 0 ? ControlStatus::Occupied : ControlStatus::Available;
    response.activeObservers = static_cast<std::uint8_t>(core_.connections().observerCount());
    response.maxObservers = static_cast<std::uint8_t>(core_.config().limits.maxObserverConnections);
    response.deviceName = core_.dids().deviceName();
    return response;
}

std::optional<ByteBuffer> SimulatorRuntime::discoveryResponse(ConstBytes datagram, const Uuid &deviceUuid) {
    FrameParser parser;
    auto pushed = parser.push(datagram);
    if (!pushed.ok() || parser.failed() || parser.bufferedFrames() == 0U) {
        return std::nullopt;
    }
    auto parsed = parser.popFrame();
    if (!parsed.ok()) {
        return std::nullopt;
    }
    const uwb::protocol::Frame frame = parsed.value();
    if (frame.type() != PayloadType::DeviceIdRequest) {
        return std::nullopt;
    }
    if (!uwb::protocol::decodeDeviceIdRequest(frame.payloadBytes()).ok()) {
        return std::nullopt;
    }

    DeviceIdResponse response = identityResponse();
    if (!deviceUuid.isZero()) {
        response.deviceUuid = deviceUuid;
    }
    const ByteBuffer payload = uwb::protocol::encodeDeviceIdResponse(response);
    auto frameResult = uwb::protocol::encodeFrame(PayloadType::DeviceIdResponse, payload);
    if (!frameResult.ok()) {
        return std::nullopt;
    }
    return frameResult.value();
}

} // namespace uwb::simulator
