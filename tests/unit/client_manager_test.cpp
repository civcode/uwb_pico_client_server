#include <catch2/catch_all.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "client_fakes.hpp"
#include "uwb/client/connection_manager.hpp"
#include "uwb/client/discovery_client.hpp"
#include "uwb/protocol/events.hpp"

using namespace uwb::client;
using namespace uwb::client::test;
using namespace uwb::protocol;
namespace domain = uwb::domain;

namespace {

constexpr std::uint16_t kServerAddress = 0x1000;

domain::DeviceUuid deviceUuidOf(std::uint8_t id) {
    return domain::uuidFromBoardId(domain::BoardId{id, 0, 0, 0, 0, 0, 0, 0});
}

DeviceIdResponse identityOf(std::uint8_t id) {
    DeviceIdResponse identity;
    identity.deviceUuid = deviceUuidOf(id);
    identity.logicalAddress = static_cast<std::uint16_t>(0x1000U + id);
    identity.tcpPort = 13401;
    identity.capabilityMask = 0x01FFU;
    identity.controlStatus = ControlStatus::Available;
    identity.maxObservers = 4;
    identity.deviceName = "device-" + std::to_string(id);
    return identity;
}

ConnectionActivationResponse acceptedActivation(const domain::DeviceUuid &uuid) {
    ConnectionActivationResponse response;
    response.serverLogicalAddress = kServerAddress;
    response.assignedRole = ConnectionRole::Control;
    response.responseCode = ActivationResponseCode::AcceptedRequestedRole;
    response.deviceUuid = uuid;
    response.serverMaxPayload = 2048;
    response.capabilityMask = 0x01FFU;
    return response;
}

// ---------------------------------------------------------------------------
// Discovery harness
// ---------------------------------------------------------------------------

// Decodes the single frame carried by one discovery datagram (§8).
inline std::optional<Frame> decodeSingle(const ByteBuffer &datagram) {
    FrameParser parser;
    static_cast<void>(parser.push(ConstBytes{datagram.data(), datagram.size()}));
    Result<Frame> frame = parser.popFrame();
    return frame.ok() ? std::optional<Frame>{frame.value()} : std::nullopt;
}

struct DiscoveryHarness {
    ManualClientClock clock;
    InlineExecutor executor;
    DiscoveryConfig config;
    std::unique_ptr<ScriptedDiscoveryTransport> owned = std::make_unique<ScriptedDiscoveryTransport>();
    ScriptedDiscoveryTransport *transport = owned.get();
    DiscoveryClient client;
    std::vector<std::pair<Endpoint, DeviceIdResponse>> seen;
    std::vector<ClientError> errors;

    explicit DiscoveryHarness(DiscoveryConfig discoveryConfig = {})
        : config(discoveryConfig), client(config, clock, executor, std::move(owned)) {
        client.setDeviceSeenCallback([this](const Endpoint &source, const DeviceIdResponse &identity) {
            seen.emplace_back(source, identity);
        });
        client.setErrorCallback([this](const ClientError &error) { errors.push_back(error); });
    }

    void tick() { client.tick(clock.nowUs()); }
    void send(std::uint64_t ms) {
        clock.advanceMs(ms);
        tick();
    }

    [[nodiscard]] std::size_t probes() const noexcept { return transport->sends().size(); }
};

// ---------------------------------------------------------------------------
// Connection manager harness
// ---------------------------------------------------------------------------

class ManagerHarness {
public:
    explicit ManagerHarness(ClientOptions clientOptions = {})
        : options(clientOptions), clock(), executor(), factory(), manager(options, clock, executor, factory) {
        manager.setHooks(
            [this](ClientConnection &connection) {
                setups.push_back(connection.state());
                ++setupsSeen;
            },
            [this](const ClientConnection &connection, const EventDelivery &delivery) {
                events.emplace_back(connection.deviceUuid(), delivery.event.eventId);
            },
            [this](const ClientConnection &connection) { states.push_back(connection.state()); },
            [this](const ClientConnection &, const ClientError &error) { errors.push_back(error); });
    }

    void tick() { manager.tick(clock.nowUs()); }
    void advanceAndTick(std::uint64_t ms) {
        clock.advanceMs(ms);
        tick();
    }

    ScriptedTransport *latestTransport() { return factory.last(); }

    // Brings one connection to the Ready state: TCP, activation request, activation response.
    void activateDevice(const std::shared_ptr<ClientConnection> &connection, std::uint64_t latencyMs = 5) {
        ScriptedTransport *transport = latestTransport();
        REQUIRE(transport != nullptr);
        transport->acceptConnect();
        clock.advanceMs(latencyMs);
        transport->deliverFrame(activationResponseFrame(acceptedActivation(connection->deviceUuid())));
    }

    ClientOptions options;
    ManualClientClock clock;
    InlineExecutor executor;
    ScriptedTransportFactory factory;
    ConnectionManager manager;

    std::vector<ConnectionState> setups;
    std::vector<ConnectionState> states;
    std::vector<ClientError> errors;
    std::vector<std::pair<domain::DeviceUuid, std::uint16_t>> events;
    std::size_t setupsSeen = 0;
};

} // namespace

// ===========================================================================
// DiscoveryClient (specification §58)
// ===========================================================================

TEST_CASE("discovery probes the broadcast targets as soon as it starts", "[unit][client]") {
    DiscoveryHarness h;
    h.client.start();

    CHECK(h.probes() == h.client.targets().size());
    CHECK_FALSE(h.client.targets().empty());
    CHECK(h.client.requestsSent() == h.client.targets().size());
    for (const Endpoint &target : h.client.targets()) {
        CHECK(target.port == 13401U);
    }
    for (const auto &probe : h.transport->sends()) {
        auto decoded = decodeSingle(probe.datagram);
        REQUIRE(decoded.has_value());
        CHECK(decoded->type() == PayloadType::DeviceIdRequest);
        CHECK(decoded->payloadBytes().empty()); // an empty Device Id Request (§16)
    }
}

TEST_CASE("discovery re-probes on the configured interval only", "[unit][client]") {
    DiscoveryConfig config;
    config.requestIntervalMs = 500;
    DiscoveryHarness h(config);
    h.client.start();
    const std::size_t initial = h.probes();

    h.send(200);
    CHECK(h.probes() == initial);

    h.send(400); // 600 ms since the last probe: one more round
    CHECK(h.probes() == initial * 2U);

    h.client.stop();
    h.send(2000);
    CHECK(h.probes() == initial * 2U); // stopped: no further probes
    CHECK_FALSE(h.client.active());
}

TEST_CASE("discovery probeNow forces the next tick to transmit", "[unit][client]") {
    DiscoveryConfig config;
    config.requestIntervalMs = 1000;
    DiscoveryHarness h(config);
    h.client.start();
    const std::size_t initial = h.probes();

    h.client.probeNow();
    h.send(10);
    CHECK(h.probes() == initial + h.client.targets().size());
}

TEST_CASE("discovery turns Device Id Responses into device sightings", "[unit][client]") {
    DiscoveryHarness h;
    h.client.start();

    const auto identity = identityOf(7);
    h.transport->deliver(Endpoint{"10.0.0.7", 13401}, deviceIdResponseFrame(identity));

    REQUIRE(h.seen.size() == 1);
    CHECK(h.seen[0].first.host == "10.0.0.7"); // the unicast source, not a broadcast address
    CHECK(h.seen[0].second.deviceUuid == identity.deviceUuid);
    CHECK(h.seen[0].second.deviceName == "device-7");
    CHECK(h.client.responsesReceived() == 1U);
    CHECK(h.client.malformedDatagrams() == 0U);
    CHECK(h.errors.empty());
}

TEST_CASE("discovery counts malformed datagrams instead of trusting them", "[unit][client]") {
    DiscoveryHarness h;
    h.client.start();

    const ByteBuffer junk{0x00, 0x01, 0x02, 0x03};
    h.transport->deliver(Endpoint{"10.0.0.7", 13401}, junk);

    const ByteBuffer alive = aliveCheckResponseFrame(0x01020304U);
    h.transport->deliver(Endpoint{"10.0.0.7", 13401}, alive); // a valid frame of the wrong type

    CHECK(h.seen.empty());
    CHECK(h.client.malformedDatagrams() == 2U);
    CHECK(h.client.responsesReceived() == 0U);
    CHECK_FALSE(h.errors.empty());
}

// ===========================================================================
// ConnectionManager (specification §55, §56)
// ===========================================================================

TEST_CASE("open allocates a distinct client logical address per device", "[unit][client]") {
    ClientOptions options;
    options.clientLogicalAddressBase = 0x0E10;
    ManagerHarness h(options);

    auto first = h.manager.open(deviceUuidOf(1), Endpoint{"10.0.0.1", 13401});
    REQUIRE(first.ok());
    h.activateDevice(first.value());
    auto second = h.manager.open(deviceUuidOf(2), Endpoint{"10.0.0.2", 13401});
    REQUIRE(second.ok());

    const std::uint16_t addressOne = first.value()->clientLogicalAddress();
    const std::uint16_t addressTwo = second.value()->clientLogicalAddress();
    CHECK(addressOne != addressTwo);
    CHECK(addressOne >= uwb::protocol::kClientLogicalAddressMin);
    CHECK(addressTwo <= uwb::protocol::kClientLogicalAddressMax);
    CHECK(h.manager.count() == 2U);
    CHECK(h.manager.findByAddress(addressOne)->deviceUuid() == deviceUuidOf(1));

    // Opening the same device twice reuses the connection instead of doubling
    // the number of sockets (§56).
    auto again = h.manager.open(deviceUuidOf(1), Endpoint{"10.0.0.1", 13401});
    REQUIRE(again.ok());
    CHECK(again.value() == first.value());
    CHECK(h.manager.count() == 2U);
    CHECK(h.factory.createdCount() == 2U); // one transport per device
}

TEST_CASE("open rejects an endpoint without a host", "[unit][client]") {
    ManagerHarness h;
    auto opened = h.manager.open(deviceUuidOf(1), Endpoint{"", 13401});
    CHECK(opened.failed());
    CHECK(static_cast<ClientErrorCode>(opened.error().code) == ClientErrorCode::InvalidArgument);
    CHECK(h.manager.count() == 0U);
}

TEST_CASE("the manager drives the activation handshake before services", "[unit][client]") {
    ManagerHarness h;
    auto opened = h.manager.open(deviceUuidOf(1), Endpoint{"10.0.0.1", 13401});
    REQUIRE(opened.ok());
    auto connection = opened.value();

    CHECK(connection->state() == ConnectionState::Connecting);

    ScriptedTransport *transport = h.latestTransport();
    REQUIRE(transport != nullptr);
    transport->acceptConnect();
    CHECK(connection->state() == ConnectionState::TcpConnected);

    auto written = firstWriteOfType(*transport, PayloadType::ConnectionActivationRequest);
    REQUIRE(written.has_value());
    auto activation = decodeConnectionActivationRequest(written->payloadBytes());
    REQUIRE(activation.ok());
    CHECK(activation.value().clientLogicalAddress == connection->clientLogicalAddress());
    CHECK(activation.value().clientInstanceUuid == h.options.clientInstanceUuid);

    // A service request before activation is refused locally (§18).
    ServiceRequest request = readDidRequest(0xF001);
    auto early = connection->sendRequest(request, [](ClientResult<ServiceResponse>) {});
    CHECK(early.failed());
    CHECK(static_cast<ClientErrorCode>(early.error().code) == ClientErrorCode::NotReady);

    h.clock.advanceMs(5);
    transport->deliverFrame(activationResponseFrame(acceptedActivation(connection->deviceUuid())));

    CHECK(connection->state() == ConnectionState::Ready);
    CHECK(connection->serverLogicalAddress() == kServerAddress);
    CHECK(connection->serverMaxPayload() == 2048U);
    CHECK(h.setupsSeen == 1U);
    CHECK_FALSE(h.states.empty());

    auto allowed = connection->sendRequest(readDidRequest(0xF001), [](ClientResult<ServiceResponse>) {});
    CHECK(allowed.ok());
}

TEST_CASE("the manager recreates the transport after a drop and keeps the connection", "[unit][client]") {
    ClientOptions options;
    options.timeouts.reconnectInitialDelayMs = 100;
    options.reconnectJitter = {}; // deterministic backoff
    ManagerHarness h(options);

    auto opened = h.manager.open(deviceUuidOf(1), Endpoint{"10.0.0.1", 13401});
    REQUIRE(opened.ok());
    auto connection = opened.value();

    h.activateDevice(connection);
    REQUIRE(connection->state() == ConnectionState::Ready);
    const std::uint64_t framesBefore = connection->diagnostics().framesReceived;
    REQUIRE(framesBefore == 1U);

    ScriptedTransport *firstTransport = h.latestTransport();
    firstTransport->drop();
    CHECK(connection->state() == ConnectionState::ReconnectWait);

    h.advanceAndTick(50); // backoff has not elapsed: no new transport yet
    CHECK(h.factory.createdCount() == 1U);

    h.advanceAndTick(100);
    CHECK(h.factory.createdCount() == 2U);

    ScriptedTransport *secondTransport = h.latestTransport();
    REQUIRE(secondTransport != nullptr);
    CHECK(secondTransport != firstTransport); // a fresh socket, the same connection object

    // Diagnostics survive the reconnect because the connection object survives.
    CHECK(connection->deviceUuid() == deviceUuidOf(1));
    CHECK(connection->diagnostics().framesReceived == framesBefore);
    CHECK(connection->diagnostics().reconnectAttempts == 1U);

    secondTransport->acceptConnect();
    h.clock.advanceMs(5);
    secondTransport->deliverFrame(activationResponseFrame(acceptedActivation(connection->deviceUuid())));
    CHECK(connection->state() == ConnectionState::Ready);
}

TEST_CASE("the manager routes device events to the owning connection", "[unit][client]") {
    ManagerHarness h;
    auto opened = h.manager.open(deviceUuidOf(1), Endpoint{"10.0.0.1", 13401});
    REQUIRE(opened.ok());
    auto connection = opened.value();
    h.activateDevice(connection);

    EventNotification event;
    event.eventId = static_cast<std::uint16_t>(EventId::UwbMeasurement);
    event.streamId = 1;
    event.sequence = 42;
    event.payload = encodeMeasurementEvent(MeasurementEvent{});
    h.latestTransport()->deliverFrame(eventNotificationFrame(event));

    REQUIRE(h.events.size() == 1);
    CHECK(h.events[0].first == connection->deviceUuid());
    CHECK(h.events[0].second == static_cast<std::uint16_t>(EventId::UwbMeasurement));
    CHECK(connection->diagnostics().eventNotifications == 1U);
}

TEST_CASE("close and closeAll disconnect devices", "[unit][client]") {
    ManagerHarness h;
    const auto uuidOne = deviceUuidOf(1);
    const auto uuidTwo = deviceUuidOf(2);

    auto first = h.manager.open(uuidOne, Endpoint{"10.0.0.1", 13401});
    auto second = h.manager.open(uuidTwo, Endpoint{"10.0.0.2", 13401});
    REQUIRE(first.ok());
    REQUIRE(second.ok());
    CHECK(h.manager.count() == 2U);

    h.manager.close(uuidOne);
    CHECK(h.manager.count() == 1U);
    CHECK(h.manager.find(uuidOne) == nullptr);
    CHECK(first.value()->state() == ConnectionState::Disconnected);
    CHECK(h.manager.findByAddress(second.value()->clientLogicalAddress()) != nullptr);

    h.manager.closeAll();
    CHECK(h.manager.count() == 0U);
    CHECK(h.manager.connections().empty());
}

TEST_CASE("manager tick supervises every connection on the single I/O thread", "[unit][client]") {
    ClientOptions options;
    options.timeouts.aliveCheckIdleMs = 100;
    ManagerHarness h(options);

    auto first = h.manager.open(deviceUuidOf(1), Endpoint{"10.0.0.1", 13401});
    REQUIRE(first.ok());
    h.activateDevice(first.value());
    auto second = h.manager.open(deviceUuidOf(2), Endpoint{"10.0.0.2", 13401});
    REQUIRE(second.ok());
    CHECK(h.factory.createdCount() == 2U);

    // Activation is per connection; each scripted device answers with its own UUID.
    for (std::size_t index = 0; index < h.factory.transports().size(); ++index) {
        const auto &lease = h.factory.transports()[index];
        const auto &connection = index == 0 ? first.value() : second.value();
        lease.transport->acceptConnect();
        h.clock.advanceMs(5);
        lease.transport->deliverFrame(activationResponseFrame(acceptedActivation(connection->deviceUuid())));
    }

    h.advanceAndTick(150); // idle past aliveCheckIdleMs: exactly one alive check per connection

    std::size_t aliveChecks = 0;
    for (const auto &lease : h.factory.transports()) {
        aliveChecks += countWritesOfType(*lease.transport, PayloadType::AliveCheckRequest);
    }
    CHECK(aliveChecks == 2U); // one per connection, both driven by the same tick
    CHECK(h.manager.activeCount() == 2U);

    h.advanceAndTick(50); // the outstanding alive check is still valid: no resend
    aliveChecks = 0;
    for (const auto &lease : h.factory.transports()) {
        aliveChecks += countWritesOfType(*lease.transport, PayloadType::AliveCheckRequest);
    }
    CHECK(aliveChecks == 2U);
}
