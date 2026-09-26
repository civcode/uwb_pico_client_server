// Phase 4 integration tests (implementation plan §25, §29).
//
// The real client core (ApplicationController, ConnectionManager, ClientConnection,
// DeviceRegistry, DiscoveryClient, NotificationQueue) runs over the real Asio
// adapters against simulator devices on real sockets. Nothing here uses the
// scripted test transports used by the unit tests.

#include "test_transport.hpp"

#include <catch2/catch_all.hpp>

#include <asio.hpp>

#include <array>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <uwb/client_net/asio_transport.hpp>

#include "uwb/client/application_controller.hpp"
#include "uwb/protocol/complete_config.hpp"
#include "uwb/protocol/did_records.hpp"
#include "uwb/protocol/dids.hpp"
#include "uwb/protocol/events.hpp"
#include "uwb/protocol/payloads.hpp"

using namespace std::chrono_literals;

namespace domain = uwb::domain;
namespace protocol = uwb::protocol;
using protocol::ByteBuffer;
using protocol::ConstBytes;
using protocol::PayloadType;
using uwb::protocol::bytesOf;
using namespace uwb::client;
using uwb::simulator::SimulatorOptions;
using uwb::test::SimulatorFixture;
using uwb::test::fastOptions;

namespace {

// ---------------------------------------------------------------------------
// Simulator side
// ---------------------------------------------------------------------------

constexpr std::uint8_t kSimIdA = 1;
constexpr std::uint8_t kSimIdB = 2;
constexpr std::uint8_t kSimIdC = 3;

SimulatorOptions simOptions(std::uint8_t id, const std::string &name) {
    SimulatorOptions options = fastOptions();
    options.boardUniqueId = std::array<std::uint8_t, 8>{id, 0, 0, 0, 0, 0, 0, id};
    options.deviceName = name;
    options.logicalAddress = static_cast<std::uint16_t>(0x1000U + id);
    options.udpPort = 0; // ephemeral: tests never collide with a real device
    options.tcpPort = 0;
    options.measurementPeriodMs = 20;
    options.rangeBaseMm = 1200;
    options.rangeNoiseMm = 100;
    return options;
}

domain::DeviceUuid simUuid(const SimulatorOptions &options) {
    auto uuid = protocol::deviceUuidFromBoardId(ConstBytes{options.boardUniqueId.data(), options.boardUniqueId.size()});
    REQUIRE(uuid.ok());
    return uuid.value();
}

// A port the test reserves so a device can be restarted at the same address.
[[nodiscard]] std::uint16_t reserveFreePort() {
    asio::io_context context;
    asio::ip::tcp::acceptor acceptor(context);
    acceptor.open(asio::ip::tcp::v4());
    acceptor.bind(asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    acceptor.close();
    return port;
}

// ---------------------------------------------------------------------------
// Half-broken device: answers discovery and Activation, then goes silent.
// Used to exercise the client timeout path (§20.3) against a real socket.
// ---------------------------------------------------------------------------

class MuteDevice {
public:
    struct Identity {
        std::array<std::uint8_t, 8> boardId{9, 0, 0, 0, 0, 0, 0, 9};
        std::string name = "mute-device";
        std::uint16_t logicalAddress = 0x1F00;
    };

    explicit MuteDevice(std::uint16_t port)
        : io(), udpSocket(io), acceptor(io), timer(io), identity() {
        std::error_code error;
        udpSocket.open(asio::ip::udp::v4(), error);
        udpSocket.bind(asio::ip::udp::endpoint(asio::ip::udp::v4(), port), error);
        lastError = error.message();
        acceptor.open(asio::ip::tcp::v4(), error);
        acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true), error);
        acceptor.bind(asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port), error);
        lastError = error.message();
        acceptor.listen(8, error);
        port_ = acceptor.local_endpoint().port();
        uuid = protocol::deviceUuidFromBoardIdU64(boardIdU64(identity.boardId));
        armUdp();
        armAccept();
    }

    ~MuteDevice() { stop(); }

    void run() { io.run(); }
    void stop() {
        stopping = true;
        asio::error_code ignored;
        acceptor.close(ignored);
        udpSocket.close(ignored);
        timer.cancel();
        io.stop();
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] const domain::DeviceUuid &deviceUuid() const noexcept { return uuid; }
    [[nodiscard]] const Identity &deviceIdentity() const noexcept { return identity; }
    [[nodiscard]] std::size_t activations() const noexcept { return activations_; }

private:
    static std::uint64_t boardIdU64(const std::array<std::uint8_t, 8> &boardId) {
        std::uint64_t value = 0;
        for (std::uint8_t byte : boardId) {
            value = (value << 8U) | byte;
        }
        return value;
    }

    void armUdp() {
        udpSocket.async_receive_from(asio::buffer(udpBuffer), udpSender,
                                    [this](std::error_code ec, std::size_t size) { onDatagram(ec, size); });
    }

    void onDatagram(std::error_code ec, std::size_t size) {
        if (stopping || ec) {
            return;
        }
        protocol::FrameParser parser;
        static_cast<void>(parser.push(ConstBytes{udpBuffer.data(), size}));
        auto frame = parser.popFrame();
        if (frame.ok() && frame.value().type() == PayloadType::DeviceIdRequest) {
            protocol::DeviceIdResponse announced{};
            announced.deviceUuid = uuid;
            announced.logicalAddress = identity.logicalAddress;
            announced.tcpPort = port_;
            announced.capabilityMask = protocol::kKnownCapabilityMask;
            announced.maxObservers = 4;
            announced.deviceName = identity.name;
            const ByteBuffer body = protocol::encodeDeviceIdResponse(announced);
            auto answer = protocol::encodeFrame(PayloadType::DeviceIdResponse, bytesOf(body));
            if (answer.ok()) {
                udpSocket.send_to(asio::buffer(answer.value()), udpSender, 0, ec);
            }
        }
        armUdp();
    }

    struct Peer {
        explicit Peer(asio::ip::tcp::socket owningSocket) : socket(std::move(owningSocket)) {}
        asio::ip::tcp::socket socket;
        std::array<std::uint8_t, 4096> buffer{};
        protocol::FrameParser parser{2048};
    };

    void armAccept() {
        acceptor.async_accept([this](std::error_code ec, asio::ip::tcp::socket socket) {
            if (stopping || ec) {
                return;
            }
            auto peer = std::make_shared<Peer>(std::move(socket));
            peers.push_back(peer);
            ++activations_;
            armRead(peer);
            armAccept();
        });
    }

    void armRead(const std::shared_ptr<Peer> &peer) {
        peer->socket.async_read_some(asio::buffer(peer->buffer),
                                     [this, peer](std::error_code ec, std::size_t size) { onRead(peer, ec, size); });
    }

    // Answers the Activation handshake, then never answers anything else.
    void onRead(const std::shared_ptr<Peer> &peer, std::error_code ec, std::size_t size) {
        if (ec || stopping) {
            return;
        }
        static_cast<void>(peer->parser.push(ConstBytes{peer->buffer.data(), size}));
        for (;;) {
            auto frame = peer->parser.popFrame();
            if (!frame.ok()) {
                break;
            }
            if (frame.value().type() != PayloadType::ConnectionActivationRequest) {
                continue; // deliberately unanswered
            }
            auto request = protocol::decodeConnectionActivationRequest(frame.value().payloadBytes());
            if (!request.ok()) {
                continue;
            }
            protocol::ConnectionActivationResponse response;
            response.serverLogicalAddress = identity.logicalAddress;
            response.assignedRole = protocol::ConnectionRole::Control;
            response.responseCode = protocol::ActivationResponseCode::AcceptedRequestedRole;
            response.deviceUuid = uuid;
            response.serverMaxPayload = 2048;
            response.capabilityMask = protocol::kKnownCapabilityMask;
            const ByteBuffer body = protocol::encodeConnectionActivationResponse(response);
            auto answer = protocol::encodeFrame(PayloadType::ConnectionActivationResponse, bytesOf(body));
            if (answer.ok()) {
                asio::error_code ignored;
                asio::write(peer->socket, asio::buffer(answer.value()), ignored);
            }
        }
        armRead(peer);
    }

    asio::io_context io;
    asio::ip::udp::socket udpSocket;
    asio::ip::tcp::acceptor acceptor;
    asio::steady_timer timer;
    Identity identity;
    domain::DeviceUuid uuid;
    std::vector<std::shared_ptr<Peer>> peers;
    std::array<std::uint8_t, 2048> udpBuffer{};
    asio::ip::udp::endpoint udpSender;
    std::uint16_t port_ = 0;
    std::size_t activations_ = 0;
    std::string lastError;
    bool stopping = false;
};

// ---------------------------------------------------------------------------
// Client side
// ---------------------------------------------------------------------------

ControllerConfig clientConfig(std::vector<Endpoint> targets) {
    ControllerConfig config;
    config.discovery.targets = std::move(targets);
    config.discovery.useBroadcastTargets = false;
    config.discovery.requestIntervalMs = 100;
    config.options.timeouts.connectTimeoutMs = 2000;
    config.options.timeouts.requestTimeoutMs = 800;
    config.options.timeouts.reconnectInitialDelayMs = 100;
    config.options.timeouts.reconnectMaxDelayMs = 400;
    config.options.timeouts.aliveCheckIdleMs = 5000;
    config.options.reconnectJitter = [](std::uint32_t) { return 0U; }; // deterministic backoff
    return config;
}

// Drives the client I/O loop on the calling thread: ClientRuntime owns one
// io_context, so polling here is what a CLI event loop would do.
class ClientHarness {
public:
    explicit ClientHarness(ControllerConfig config) : runtime(std::move(config)) { runtime.start(); }

    ~ClientHarness() {
        runtime.controller().disconnectAll();
        runtime.stop();
    }

    ClientHarness(const ClientHarness &) = delete;
    ClientHarness &operator=(const ClientHarness &) = delete;

    void pump(std::chrono::milliseconds duration) {
        const auto deadline = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < deadline) {
            runtime.poll();
            std::this_thread::sleep_for(1ms);
        }
    }

    template <typename Predicate>
    [[nodiscard]] bool waitFor(Predicate predicate, std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            runtime.poll();
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return predicate();
    }

    [[nodiscard]] std::optional<DeviceSummary> summary(const domain::DeviceUuid &uuid) {
        return runtime.controller().deviceSummary(uuid);
    }

    [[nodiscard]] bool waitForState(const domain::DeviceUuid &uuid, ConnectionState state,
                                    std::chrono::milliseconds timeout = 3000ms) {
        return waitFor([&] {
            auto found = runtime.controller().deviceSummary(uuid);
            return found.has_value() && found->state == state;
        }, timeout);
    }

    // Collects notifications while a predicate is not yet satisfied.
    struct Collected {
        std::vector<ClientNotification> all;
        std::size_t measurements = 0;
        std::map<domain::DeviceUuid, std::size_t> measurementsPerDevice;
        std::size_t dropped = 0;
    };

    Collected collect(std::chrono::milliseconds duration) {
        Collected out;
        const auto deadline = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < deadline) {
            runtime.poll();
            ClientNotification note;
            while (runtime.controller().notifications().tryPop(note)) {
                if (note.kind == NotificationKind::Measurement) {
                    out.measurements++;
                    if (note.measurement.has_value()) {
                        out.measurementsPerDevice[note.measurement->gatewayUuid]++;
                    }
                }
                out.all.push_back(std::move(note));
            }
            std::this_thread::sleep_for(1ms);
        }
        out.dropped = runtime.controller().diagnostics().notificationsDropped;
        return out;
    }

    [[nodiscard]] ApplicationController &controller() noexcept { return runtime.controller(); }
    uwb::client_net::ClientRuntime runtime;
};

// Unwraps a ReadDid response into the raw record bytes (§21), or nullopt when the
// transaction failed.
[[nodiscard]] std::optional<ByteBuffer> readDidRecord(const ClientResult<ServiceResponse> &response) {
    if (!response.ok()) {
        return std::nullopt;
    }
    auto decoded = protocol::decodeReadDidResponse(response.value().pduBytes());
    if (!decoded.ok()) {
        return std::nullopt;
    }
    return decoded.value().data;
}

// Writes and raw AT require the Extended Session (§22.3, §30).
void enterExtendedSession(ClientHarness &harness, const domain::DeviceUuid &uuid) {
    bool done = false;
    bool ok = false;
    harness.controller().requestSession(uuid, protocol::SessionId::Extended, [&](ClientResult<std::string> result) {
        done = true;
        ok = result.ok();
    });
    REQUIRE(harness.waitFor([&] { return done; }, 3000ms));
    REQUIRE(ok);
}

// Starts a live measurement stream and waits for the command result.
void startStream(ClientHarness &harness, const domain::DeviceUuid &uuid) {
    bool done = false;
    bool ok = false;
    harness.controller().startMeasurementStream(uuid, protocol::StreamMode::Live,
                                                [&](ClientResult<std::string> result) {
                                                    done = true;
                                                    ok = result.ok();
                                                });
    REQUIRE(harness.waitFor([&] { return done; }, 3000ms));
    REQUIRE(ok);
}

void connectAll(ClientHarness &harness, const std::vector<std::unique_ptr<SimulatorFixture>> &sims) {
    for (const auto &sim : sims) {
        const auto uuid = simUuid(sim->options);
        REQUIRE(harness.controller().connectDevice(uuid).ok());
        REQUIRE(harness.waitForState(uuid, ConnectionState::Ready));
    }
}

} // namespace

// ===========================================================================
// Discovery over real UDP (specification §17, §58)
// ===========================================================================

TEST_CASE("the client discovers every simulator and keys the registry by UUID", "[integration][client]") {
    std::vector<std::unique_ptr<SimulatorFixture>> sims;
    sims.push_back(std::make_unique<SimulatorFixture>(simOptions(kSimIdA, "alpha")));
    sims.push_back(std::make_unique<SimulatorFixture>(simOptions(kSimIdB, "bravo")));
    sims.push_back(std::make_unique<SimulatorFixture>(simOptions(kSimIdC, "charlie")));

    std::vector<Endpoint> targets;
    for (const auto &sim : sims) {
        targets.push_back(Endpoint{"127.0.0.1", sim->udpPort()});
    }

    ClientHarness harness(clientConfig(targets));
    harness.controller().discoverNow();

    REQUIRE(harness.waitFor([&] { return harness.controller().listDevices().size() >= 3U; }, 3000ms));

    const auto devices = harness.controller().listDevices();
    CHECK(devices.size() == 3U);

    for (const auto &sim : sims) {
        const auto uuid = simUuid(sim->options);
        auto found = harness.summary(uuid);
        REQUIRE(found.has_value());
        CHECK(found->name == sim->options.deviceName);
        CHECK(found->logicalAddress == sim->options.logicalAddress);
        // The TCP endpoint must carry the port the device actually reported.
        CHECK(found->endpoint.port == sim->tcpPort());
        CHECK(found->capabilities != 0U);
    }

    const auto snapshot = harness.controller().diagnostics();
    CHECK(snapshot.discovered == 3U);
    CHECK(snapshot.discoveryResponses >= 3U);
    CHECK(snapshot.discoveryRequestsSent >= 3U);
    CHECK(snapshot.malformedDatagrams == 0U);
}

TEST_CASE("resolveDevice accepts a device name or a UUID string", "[integration][client]") {
    std::vector<std::unique_ptr<SimulatorFixture>> sims;
    sims.push_back(std::make_unique<SimulatorFixture>(simOptions(kSimIdA, "alpha")));

    ClientHarness harness(clientConfig({Endpoint{"127.0.0.1", sims.front()->udpPort()}}));
    harness.pump(500ms);

    const auto uuid = simUuid(sims.front()->options);
    auto byName = harness.controller().resolveDevice("alpha");
    REQUIRE(byName.has_value());
    CHECK(*byName == uuid);

    auto byUuid = harness.controller().resolveDevice(domain::uuidToString(uuid));
    REQUIRE(byUuid.has_value());
    CHECK(*byUuid == uuid);

    CHECK_FALSE(harness.controller().resolveDevice("does-not-exist").has_value());
}

TEST_CASE("connect activates the device and publishes the state change", "[integration][client]") {
    std::vector<std::unique_ptr<SimulatorFixture>> sims;
    sims.push_back(std::make_unique<SimulatorFixture>(simOptions(kSimIdA, "alpha")));

    ClientHarness harness(clientConfig({Endpoint{"127.0.0.1", sims.front()->udpPort()}}));
    harness.pump(400ms);

    const auto uuid = simUuid(sims.front()->options);
    REQUIRE(harness.controller().connectDevice(uuid).ok());

    auto seen = harness.collect(600ms);
    bool sawStateChange = false;
    bool sawDeviceSeen = false;
    for (const ClientNotification &note : seen.all) {
        if (note.kind == NotificationKind::DeviceStateChanged) {
            sawStateChange = true;
        }
        if (note.kind == NotificationKind::DeviceSeen) {
            sawDeviceSeen = true;
        }
    }
    CHECK(sawDeviceSeen); // the discovery sighting is published (§59)
    CHECK(sawStateChange);
    REQUIRE(harness.waitForState(uuid, ConnectionState::Ready));

    auto found = harness.summary(uuid);
    REQUIRE(found.has_value());
    CHECK(found->role == protocol::ConnectionRole::Control); // first client gets Control (§18.2)
    CHECK(found->state == ConnectionState::Ready);
    CHECK(harness.controller().diagnostics().activeConnections == 1U);
}

TEST_CASE("connecting by endpoint probes the address and then activates", "[integration][client]") {
    // A device listens on one port for both discovery and TCP, so the address a
    // user types ("connect to 127.0.0.1:PORT") is the address that gets probed.
    const std::uint16_t port = reserveFreePort();
    SimulatorOptions options = simOptions(kSimIdA, "alpha");
    options.udpPort = port;
    options.tcpPort = port;
    SimulatorFixture sim(options);

    // No periodic discovery targets: only the requested endpoint may be probed.
    ClientHarness harness(clientConfig({}));
    auto result = harness.controller().connectEndpoint(Endpoint{"127.0.0.1", port}, "alpha");
    REQUIRE(result.ok());

    const auto uuid = simUuid(options);
    REQUIRE(harness.waitForState(uuid, ConnectionState::Ready, 4000ms));
    auto found = harness.summary(uuid);
    REQUIRE(found.has_value());
    CHECK(found->name == "alpha");
    CHECK(found->endpoint.port == sim.tcpPort());
}

// ===========================================================================
// Services through the client core (specification §25, §40)
// ===========================================================================

TEST_CASE("DID reads through the client core return the device values", "[integration][client]") {
    std::vector<std::unique_ptr<SimulatorFixture>> sims;
    sims.push_back(std::make_unique<SimulatorFixture>(simOptions(kSimIdA, "alpha")));

    ClientHarness harness(clientConfig({Endpoint{"127.0.0.1", sims.front()->udpPort()}}));
    harness.pump(400ms);
    connectAll(harness, sims);

    const auto uuid = simUuid(sims.front()->options);

    std::optional<std::string> name;
    bool nameDone = false;
    harness.controller().readDid(uuid, protocol::Did::DeviceName, [&](ClientResult<ServiceResponse> response) {
        const auto record = readDidRecord(response);
        if (record.has_value()) {
            auto text = protocol::decodeTextRecord8(*record, protocol::kMaxDeviceNameLength);
            if (text.ok()) {
                name = std::string(text.value().begin(), text.value().end());
            }
        }
        nameDone = true;
    });
    REQUIRE(harness.waitFor([&] { return nameDone; }, 3000ms));
    REQUIRE(name.has_value());
    CHECK(*name == "alpha");

    // A backend-backed DID takes the 0x78 interim path; the client core must keep
    // the transaction open until the final response arrives (§21.3).
    std::optional<std::uint64_t> capabilities;
    bool capsDone = false;
    harness.controller().readDid(uuid, protocol::Did::EffectiveCapabilities, [&](ClientResult<ServiceResponse> response) {
        const auto record = readDidRecord(response);
        if (record.has_value()) {
            auto decoded = protocol::decodeU64Record(*record);
            if (decoded.ok()) {
                capabilities = decoded.value();
            }
        }
        capsDone = true;
    });
    REQUIRE(harness.waitFor([&] { return capsDone; }, 3000ms));
    REQUIRE(capabilities.has_value());
    CHECK(*capabilities == sims.front()->options.effectiveMask());

    const auto snapshot = harness.controller().diagnostics();
    CHECK(snapshot.requestsSent >= 2U);
    CHECK(snapshot.responsesMatched >= 2U);
    CHECK(snapshot.negativeResponses == 0U);
    CHECK(snapshot.requestTimeouts == 0U);
}

TEST_CASE("configuration read write and save round-trips through the client", "[integration][client]") {
    std::vector<std::unique_ptr<SimulatorFixture>> sims;
    sims.push_back(std::make_unique<SimulatorFixture>(simOptions(kSimIdA, "alpha")));

    ClientHarness harness(clientConfig({Endpoint{"127.0.0.1", sims.front()->udpPort()}}));
    harness.pump(400ms);
    connectAll(harness, sims);

    const auto uuid = simUuid(sims.front()->options);

    auto readConfig = [&](protocol::CompleteUwbConfig &out) {
        bool done = false;
        bool ok = false;
        harness.controller().readConfiguration(uuid, [&](ClientResult<ServiceResponse> response) {
            const auto record = readDidRecord(response);
            if (record.has_value()) {
                auto decoded = protocol::decodeCompleteUwbConfig(bytesOf(*record));
                if (decoded.ok()) {
                    out = std::move(decoded.value());
                    ok = true;
                }
            }
            done = true;
        });
        REQUIRE(harness.waitFor([&] { return done; }, 3000ms));
        REQUIRE(ok);
    };

    protocol::CompleteUwbConfig before;
    readConfig(before);

    enterExtendedSession(harness, uuid);

    protocol::UwbDeviceParametersRecord record;
    record.id = 1;
    record.role = 1;
    record.channel = 7;
    record.rate = 0;

    protocol::CompleteUwbConfig updated;
    updated.entries.push_back(protocol::DidRecordEntry{
        static_cast<std::uint16_t>(protocol::Did::UwbDeviceParameters), protocol::encodeRecord(record)});

    bool writeDone = false;
    ClientResult<std::string> writeResult = ClientResult<std::string>::ok("unset");
    harness.controller().writeConfiguration(uuid, updated, [&](ClientResult<std::string> result) {
        writeDone = true;
        writeResult = std::move(result);
    });
    REQUIRE(harness.waitFor([&] { return writeDone; }, 3000ms));
    INFO(writeResult.error().toString());
    REQUIRE(writeResult.ok());

    bool saveDone = false;
    bool saveOk = false;
    harness.controller().saveConfiguration(uuid, [&](ClientResult<std::string> result) {
        saveDone = true;
        saveOk = result.ok();
    });
    REQUIRE(harness.waitFor([&] { return saveDone; }, 3000ms));
    REQUIRE(saveOk);

    protocol::CompleteUwbConfig after;
    readConfig(after);

    bool channelSeen = false;
    for (const auto &entry : after.entries) {
        if (entry.did == static_cast<std::uint16_t>(protocol::Did::UwbDeviceParameters)) {
            auto stored = protocol::decodeUwbDeviceParametersRecord(entry.data);
            REQUIRE(stored.ok());
            CHECK(stored.value().channel == 7);
            channelSeen = true;
        }
    }
    CHECK(channelSeen);
    CHECK(!after.entries.empty());
    static_cast<void>(before);
}

TEST_CASE("a service error surfaces as a client error with the device NRC", "[integration][client]") {
    SimulatorOptions options = simOptions(kSimIdA, "alpha");
    options.rawAtEnabled = false; // Observer-free: the client is Control, but AT is disabled
    std::vector<std::unique_ptr<SimulatorFixture>> sims;
    sims.push_back(std::make_unique<SimulatorFixture>(options));

    ClientHarness harness(clientConfig({Endpoint{"127.0.0.1", sims.front()->udpPort()}}));
    harness.pump(400ms);
    connectAll(harness, sims);

    const auto uuid = simUuid(sims.front()->options);

    ClientResult<std::string> result = ClientResult<std::string>::ok("unset");
    harness.controller().executeAtCommand(uuid, "AT+ID", [&](ClientResult<std::string> response) { result = std::move(response); });
    REQUIRE(harness.waitFor([&] { return result.failed(); }, 3000ms));

    CHECK(result.failed());
    CHECK(static_cast<ClientErrorCode>(result.error().code) == ClientErrorCode::ServiceRejected);
    CHECK(result.error().nrc.has_value());
    // §25: raw AT is configuration-gated, so the device answers 0x7F.
    CHECK(*result.error().nrc == static_cast<std::uint8_t>(protocol::ServiceNrc::ServiceNotSupportedInActiveSession));
    CHECK(harness.controller().diagnostics().negativeResponses >= 1U);
}

TEST_CASE("an extended session unlocks raw AT passthrough", "[integration][client]") {
    std::vector<std::unique_ptr<SimulatorFixture>> sims;
    sims.push_back(std::make_unique<SimulatorFixture>(simOptions(kSimIdA, "alpha")));

    ClientHarness harness(clientConfig({Endpoint{"127.0.0.1", sims.front()->udpPort()}}));
    harness.pump(400ms);
    connectAll(harness, sims);

    const auto uuid = simUuid(sims.front()->options);

    bool sessionDone = false;
    ClientResult<std::string> sessionResult = ClientResult<std::string>::ok("unset");
    harness.controller().requestSession(uuid, protocol::SessionId::Extended, [&](ClientResult<std::string> response) {
        sessionResult = std::move(response);
        sessionDone = true;
    });
    REQUIRE(harness.waitFor([&] { return sessionDone; }, 3000ms));
    REQUIRE(sessionResult.ok());
    REQUIRE(harness.summary(uuid));
    CHECK(harness.summary(uuid)->session == protocol::SessionId::Extended);

    ClientResult<std::string> atResult = ClientResult<std::string>::ok("unset");
    bool atDone = false;
    harness.controller().executeAtCommand(uuid, "AT+ID", [&](ClientResult<std::string> response) {
        atResult = std::move(response);
        atDone = true;
    });
    REQUIRE(harness.waitFor([&] { return atDone; }, 4000ms));
    REQUIRE(atResult.ok());
    CHECK_FALSE(atResult.value().empty()); // the raw AT text is handed to the caller
}

// ===========================================================================
// Streaming and notifications (specification §31, §35, §59)
// ===========================================================================

TEST_CASE("live streams from several devices arrive on the notification queue", "[integration][client]") {
    std::vector<std::unique_ptr<SimulatorFixture>> sims;
    sims.push_back(std::make_unique<SimulatorFixture>(simOptions(kSimIdA, "alpha")));
    sims.push_back(std::make_unique<SimulatorFixture>(simOptions(kSimIdB, "bravo")));
    sims.push_back(std::make_unique<SimulatorFixture>(simOptions(kSimIdC, "charlie")));

    std::vector<Endpoint> targets;
    for (const auto &sim : sims) {
        targets.push_back(Endpoint{"127.0.0.1", sim->udpPort()});
    }

    ClientHarness harness(clientConfig(targets));
    harness.pump(600ms);
    connectAll(harness, sims);

    for (const auto &sim : sims) {
        startStream(harness, simUuid(sim->options));
    }

    auto collected = harness.collect(1200ms);

    CHECK(collected.measurements >= 10U);
    CHECK(collected.measurementsPerDevice.size() == 3U); // three devices, one I/O thread
    for (const auto &note : collected.all) {
        if (note.kind != NotificationKind::Measurement || !note.measurement.has_value()) {
            continue;
        }
        const auto &sample = *note.measurement;
        REQUIRE(sample.rawRangeMm.has_value());
        // Simulated range is 1200 mm +/- 100 mm; a decoded value outside that band
        // would mean the wire decoding is wrong.
        CHECK(*sample.rawRangeMm >= 1100);
        CHECK(*sample.rawRangeMm <= 1300);
    }

    const auto snapshot = harness.controller().diagnostics();
    CHECK(snapshot.activeConnections == 3U);
    CHECK(snapshot.framesReceived > snapshot.framesSent); // the devices push measurements
}

TEST_CASE("stopping a stream stops the measurements for that device", "[integration][client]") {
    std::vector<std::unique_ptr<SimulatorFixture>> sims;
    sims.push_back(std::make_unique<SimulatorFixture>(simOptions(kSimIdA, "alpha")));

    ClientHarness harness(clientConfig({Endpoint{"127.0.0.1", sims.front()->udpPort()}}));
    harness.pump(400ms);
    connectAll(harness, sims);

    const auto uuid = simUuid(sims.front()->options);
    startStream(harness, uuid);
    CHECK(harness.summary(uuid)->subscriptions == 1U);

    auto first = harness.collect(400ms);
    CHECK(first.measurements > 0U);

    bool stopped = false;
    bool stopOk = false;
    harness.controller().stopMeasurementStream(uuid, [&](ClientResult<std::string> result) {
        stopped = true;
        stopOk = result.ok();
    });
    REQUIRE(harness.waitFor([&] { return stopped; }, 3000ms));
    REQUIRE(stopOk);
    REQUIRE(harness.waitFor([&] { return harness.summary(uuid)->subscriptions == 0U; }, 2000ms));

    // Drain whatever was still in flight, then measure the quiet period.
    harness.collect(200ms);
    auto quiet = harness.collect(500ms);
    CHECK(quiet.measurements == 0U);
}

TEST_CASE("a slow notification consumer drops notifications, not the connection", "[integration][client]") {
    std::vector<std::unique_ptr<SimulatorFixture>> sims;
    sims.push_back(std::make_unique<SimulatorFixture>(simOptions(kSimIdA, "alpha")));

    ControllerConfig config = clientConfig({Endpoint{"127.0.0.1", sims.front()->udpPort()}});
    config.notificationCapacity = 4; // deliberately tiny queue
    ClientHarness harness(config);
    harness.pump(400ms);
    connectAll(harness, sims);

    const auto uuid = simUuid(sims.front()->options);
    startStream(harness, uuid);

    // Nobody drains the queue while the device streams.
    harness.pump(800ms);

    const auto snapshot = harness.controller().diagnostics();
    CHECK(snapshot.notificationsDropped > 0U);

    // The connection itself stayed healthy: a command still completes.
    bool done = false;
    bool ok = false;
    harness.controller().readDid(uuid, protocol::Did::DeviceName, [&](ClientResult<ServiceResponse> response) {
        done = true;
        ok = response.ok();
    });
    REQUIRE(harness.waitFor([&] { return done; }, 3000ms));
    CHECK(ok);
    CHECK(harness.waitForState(uuid, ConnectionState::Ready, 1000ms));
}

// ===========================================================================
// Reconnect and supervision (specification §56, §73)
// ===========================================================================

TEST_CASE("a dropped device reconnects and its stream is restored", "[integration][client]") {
    const std::uint16_t port = reserveFreePort();

    SimulatorOptions first = simOptions(kSimIdA, "alpha");
    first.udpPort = port;
    first.tcpPort = port;
    auto sim = std::make_unique<SimulatorFixture>(first);

    ClientHarness harness(clientConfig({Endpoint{"127.0.0.1", port}}));
    harness.pump(600ms);

    const auto uuid = simUuid(first);
    REQUIRE(harness.controller().connectDevice(uuid).ok());
    REQUIRE(harness.waitForState(uuid, ConnectionState::Ready));
    startStream(harness, uuid);
    CHECK(harness.collect(400ms).measurements > 0U);

    // The device goes away: the socket closes and the client must keep retrying.
    sim.reset();

    REQUIRE(harness.waitFor([&] {
        auto found = harness.summary(uuid);
        return found.has_value() && (found->state == ConnectionState::ReconnectWait || found->state == ConnectionState::Connecting);
    }, 3000ms));

    // The same device comes back on the same address.
    SimulatorOptions second = first;
    sim = std::make_unique<SimulatorFixture>(second);

    REQUIRE(harness.waitForState(uuid, ConnectionState::Ready, 6000ms));

    const auto snapshot = harness.controller().diagnostics();
    CHECK(snapshot.reconnectAttempts >= 1U);

    // Subscriptions are re-established without the application asking again (§56).
    auto collected = harness.collect(600ms);
    CHECK(collected.measurements > 0U);
    CHECK(harness.summary(uuid)->subscriptions == 1U);

    sim->serverRef().requestStop();
}

TEST_CASE("an unanswered request times out and frees the pending slot", "[integration][client]") {
    const std::uint16_t port = reserveFreePort();
    MuteDevice device(port);
    std::thread runner([&device] { device.run(); });

    ControllerConfig config = clientConfig({Endpoint{"127.0.0.1", port}});
    config.options.timeouts.requestTimeoutMs = 400;
    ClientHarness harness(config);

    auto connectResult = harness.controller().connectEndpoint(Endpoint{"127.0.0.1", port}, "mute");
    REQUIRE(connectResult.ok());
    const auto uuid = device.deviceUuid();
    REQUIRE(harness.waitForState(uuid, ConnectionState::Ready, 4000ms));

    bool done = false;
    ClientResult<ServiceResponse> result = ClientResult<ServiceResponse>::ok(ServiceResponse{});
    harness.controller().readDid(uuid, protocol::Did::DeviceName, [&](ClientResult<ServiceResponse> response) {
        result = std::move(response);
        done = true;
    });

    REQUIRE(harness.waitFor([&] { return done; }, 4000ms));
    CHECK(result.failed());
    CHECK(static_cast<ClientErrorCode>(result.error().code) == ClientErrorCode::Timeout);
    CHECK(harness.controller().diagnostics().requestTimeouts >= 1U);

    // The pending table was freed, so the next request is accepted rather than
    // rejected as TooManyOutstanding.
    bool secondDone = false;
    bool secondRejected = false;
    harness.controller().readDid(uuid, protocol::Did::DeviceName, [&](ClientResult<ServiceResponse> response) {
        secondDone = true;
        static_cast<void>(response);
    });
    REQUIRE(harness.waitFor([&] { return secondDone; }, 3000ms));
    CHECK_FALSE(secondRejected);
    CHECK(harness.controller().diagnostics().requestsSent >= 2U);

    harness.controller().disconnectAll();
    device.stop();
    runner.join();
}

TEST_CASE("disconnect closes the device and clears it from the active set", "[integration][client]") {
    std::vector<std::unique_ptr<SimulatorFixture>> sims;
    sims.push_back(std::make_unique<SimulatorFixture>(simOptions(kSimIdA, "alpha")));

    ClientHarness harness(clientConfig({Endpoint{"127.0.0.1", sims.front()->udpPort()}}));
    harness.pump(400ms);
    connectAll(harness, sims);

    const auto uuid = simUuid(sims.front()->options);
    CHECK(harness.controller().diagnostics().activeConnections == 1U);
    CHECK(sims.front()->serverRef().connectionCount() == 1U);

    harness.controller().disconnectDevice(uuid);

    REQUIRE(harness.waitFor([&] { return sims.front()->serverRef().connectionCount() == 0U; }, 3000ms));
    CHECK(harness.controller().diagnostics().activeConnections == 0U);
    auto found = harness.summary(uuid);
    REQUIRE(found.has_value());
    CHECK(found->state == ConnectionState::Disconnected);

    // The device is still known from discovery, so reconnecting works without a
    // new discovery round.
    REQUIRE(harness.controller().connectDevice(uuid).ok());
    CHECK(harness.waitForState(uuid, ConnectionState::Ready, 3000ms));
}

TEST_CASE("routines run through the client core and report completion", "[integration][client]") {
    std::vector<std::unique_ptr<SimulatorFixture>> sims;
    sims.push_back(std::make_unique<SimulatorFixture>(simOptions(kSimIdA, "alpha")));

    ClientHarness harness(clientConfig({Endpoint{"127.0.0.1", sims.front()->udpPort()}}));
    harness.pump(400ms);
    connectAll(harness, sims);

    const auto uuid = simUuid(sims.front()->options);

    bool done = false;
    ClientResult<std::string> result = ClientResult<std::string>::ok("unset");
    harness.controller().startRoutine(uuid, protocol::RoutineId::SaveUwbConfiguration, protocol::ByteBuffer{},
                                      [&](ClientResult<std::string> response) {
                                          result = std::move(response);
                                          done = true;
                                      });
    REQUIRE(harness.waitFor([&] { return done; }, 5000ms));
    REQUIRE(result.ok());

    auto collected = harness.collect(400ms);
    bool sawCommandFinished = false;
    for (const ClientNotification &note : collected.all) {
        if (note.kind == NotificationKind::CommandFinished) {
            sawCommandFinished = true;
        }
    }
    CHECK(sawCommandFinished);
}
