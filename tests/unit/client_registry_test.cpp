#include <catch2/catch_all.hpp>

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "client_fakes.hpp"
#include "uwb/client/device_registry.hpp"
#include "uwb/client/notification_queue.hpp"

using namespace uwb::client;
using namespace uwb::client::test;
using namespace uwb::protocol;
namespace domain = uwb::domain;

namespace {

DeviceIdResponse identityOf(std::uint8_t id, const std::string &name) {
    DeviceIdResponse identity;
    identity.deviceUuid = domain::uuidFromBoardId(domain::BoardId{id, 0, 0, 0, 0, 0, 0, 0});
    identity.logicalAddress = static_cast<std::uint16_t>(0x1000U + id);
    identity.tcpPort = 13401;
    identity.capabilityMask = 0x01FFU;
    identity.controlStatus = ControlStatus::Available;
    identity.maxObservers = 4;
    identity.deviceName = name;
    return identity;
}

domain::DeviceUuid uuidOf(std::uint8_t id) {
    return domain::uuidFromBoardId(domain::BoardId{id, 0, 0, 0, 0, 0, 0, 0});
}

} // namespace

TEST_CASE("the discovery registry is keyed by device uuid", "[unit][client]") {
    DeviceRegistry registry(5000);

    registry.updateSeen(identityOf(1, "anchor-a"), Endpoint{"192.168.1.10", 13401}, 1'000'000ULL);
    registry.updateSeen(identityOf(1, "anchor-a"), Endpoint{"192.168.1.11", 13401}, 1'100'000ULL);
    registry.updateSeen(identityOf(2, "anchor-b"), Endpoint{"192.168.1.12", 13401}, 1'200'000ULL);

    CHECK(registry.size() == 2U);
    CHECK(registry.contains(uuidOf(1)));

    const DeviceEntry *entry = registry.find(uuidOf(1));
    REQUIRE(entry != nullptr);
    CHECK(entry->uuid == uuidOf(1));
    CHECK(entry->sightings == 2U);
    CHECK(entry->firstSeenUs == 1'000'000ULL);
    CHECK(entry->lastSeenUs == 1'100'000ULL);
    CHECK(entry->identity.deviceName == "anchor-a");

    // A repeated sighting of the same UUID refreshes the endpoint, so a device
    // that moved in the network is reachable at its new address.
    CHECK(entry->discoveryEndpoint.host == "192.168.1.11");
}

TEST_CASE("the TCP endpoint comes from the response port and the datagram source", "[unit][client]") {
    DeviceRegistry registry(5000);
    DeviceIdResponse identity = identityOf(3, "anchor-c");
    identity.tcpPort = 13555;

    registry.updateSeen(identity, Endpoint{"10.0.0.7", 13401}, 1'000'000ULL);

    const DeviceEntry *entry = registry.find(uuidOf(3));
    REQUIRE(entry != nullptr);
    CHECK(entry->tcpEndpoint.host == "10.0.0.7");
    CHECK(entry->tcpEndpoint.port == 13555U);
    CHECK(entry->discoveryEndpoint.port == 13401U);
}

TEST_CASE("a device with a live connection keeps the endpoint it connected to", "[unit][client]") {
    DeviceRegistry registry(5000);
    registry.updateSeen(identityOf(4, "anchor-d"), Endpoint{"10.0.0.1", 13401}, 1'000'000ULL);

    ManualClientClock clock;
    InlineExecutor executor;
    ConnectionOptions options = testConnectionOptions();
    auto connection = std::make_shared<ClientConnection>(options, clock, executor);

    registry.attachConnection(uuidOf(4), connection);
    registry.setConnectionState(uuidOf(4), ConnectionState::Ready);
    CHECK(registry.find(uuidOf(4))->managed);

    // A stale broadcast from another interface must not hijack the endpoint.
    registry.updateSeen(identityOf(4, "anchor-d"), Endpoint{"10.0.0.99", 13401}, 1'200'000ULL);
    CHECK(registry.find(uuidOf(4))->tcpEndpoint.host == "10.0.0.1");

    registry.detachConnection(uuidOf(4));
    CHECK_FALSE(registry.find(uuidOf(4))->managed);
    CHECK(registry.find(uuidOf(4))->connection == nullptr);
}

TEST_CASE("registry lookups by name and logical address resolve discovered devices", "[unit][client]") {
    DeviceRegistry registry(5000);
    registry.updateSeen(identityOf(1, "anchor-a"), Endpoint{"10.0.0.1", 13401}, 1'000'000ULL);
    registry.updateSeen(identityOf(2, "anchor-b"), Endpoint{"10.0.0.2", 13401}, 1'000'000ULL);

    REQUIRE(registry.byName("anchor-b") != nullptr);
    CHECK(registry.byName("anchor-b")->uuid == uuidOf(2));
    CHECK(registry.byName("nope") == nullptr);

    REQUIRE(registry.byLogicalAddress(0x1002) != nullptr);
    CHECK(registry.byLogicalAddress(0x1002)->identity.deviceName == "anchor-b");
    CHECK(registry.byLogicalAddress(0x1234) == nullptr);

    CHECK(registry.all().size() == 2U);
    CHECK(registry.connected().empty());
}

TEST_CASE("stale devices are pruned, connected devices are never pruned", "[unit][client]") {
    DeviceRegistry registry(1000);

    registry.updateSeen(identityOf(1, "gone"), Endpoint{"10.0.0.1", 13401}, 1'000'000ULL);
    registry.updateSeen(identityOf(2, "seen"), Endpoint{"10.0.0.2", 13401}, 1'000'000ULL);
    registry.updateSeen(identityOf(3, "held"), Endpoint{"10.0.0.3", 13401}, 1'000'000ULL);

    ManualClientClock clock;
    InlineExecutor executor;
    auto connection = std::make_shared<ClientConnection>(testConnectionOptions(), clock, executor);
    registry.attachConnection(uuidOf(3), connection);
    registry.setConnectionState(uuidOf(3), ConnectionState::Ready);

    // Only device 2 is refreshed inside the stale window.
    registry.updateSeen(identityOf(2, "seen"), Endpoint{"10.0.0.2", 13401}, 1'900'000ULL);

    CHECK(registry.pruneStale(2'100'000ULL) == 1U);
    CHECK_FALSE(registry.contains(uuidOf(1)));
    CHECK(registry.contains(uuidOf(2)));
    CHECK(registry.contains(uuidOf(3))); // has a connection: never pruned (§58)
}

TEST_CASE("the registry caches the last known device state for the CLI", "[unit][client]") {
    DeviceRegistry registry(5000);
    registry.updateSeen(identityOf(5, "anchor-e"), Endpoint{"10.0.0.5", 13401}, 1'000'000ULL);

    ConnectionStatusRecord status;
    status.controlOccupied = 1;
    status.activeObservers = 2;
    status.maxObservers = 4;
    status.activeSession = static_cast<std::uint8_t>(SessionId::Extended);
    registry.setStatus(uuidOf(5), status);

    const DeviceEntry *entry = registry.find(uuidOf(5));
    REQUIRE(entry != nullptr);
    REQUIRE(entry->status.has_value());
    CHECK(entry->status->activeObservers == 2U);
    CHECK(entry->status->activeSession == static_cast<std::uint8_t>(SessionId::Extended));
    CHECK_FALSE(entry->configuration.has_value());

    domain::UwbConfiguration configuration;
    registry.setConfiguration(uuidOf(5), configuration);
    REQUIRE(registry.find(uuidOf(5))->configuration.has_value());
}

TEST_CASE("the notification queue is bounded and drops the oldest notification", "[unit][client]") {
    NotificationQueue queue(3);

    for (std::uint32_t value = 1; value <= 5U; ++value) {
        ClientNotification note;
        note.kind = NotificationKind::Measurement;
        note.text = std::to_string(value);
        queue.push(std::move(note));
    }

    CHECK(queue.size() == 3U);
    CHECK(queue.dropped() == 2U);

    ClientNotification note;
    REQUIRE(queue.tryPop(note));
    CHECK(note.text.value() == "3"); // 1 and 2 were dropped, the newest survive
}

TEST_CASE("the notification queue wakes a waiting consumer", "[unit][client]") {
    NotificationQueue queue(8);

    ClientNotification note;
    CHECK_FALSE(queue.waitPop(note, std::chrono::milliseconds(5))); // empty queue times out

    std::thread producer([&queue] {
        ClientNotification pushed;
        pushed.kind = NotificationKind::DeviceSeen;
        pushed.text = "device";
        queue.push(std::move(pushed));
    });

    REQUIRE(queue.waitPop(note, std::chrono::seconds(2)));
    CHECK(note.kind == NotificationKind::DeviceSeen);
    CHECK(note.text.value() == "device");
    producer.join();
}

TEST_CASE("closing the notification queue releases waiting consumers", "[unit][client]") {
    NotificationQueue queue(4);

    std::thread closer([&queue] { queue.close(); });

    ClientNotification note;
    CHECK_FALSE(queue.waitPop(note, std::chrono::seconds(2)));
    CHECK(queue.closed());
    closer.join();
}

TEST_CASE("a slow consumer drops notifications instead of blocking the producer", "[unit][client]") {
    NotificationQueue queue(16);

    // The I/O thread push must never block: it drops when the consumer lags.
    for (int attempt = 0; attempt < 100; ++attempt) {
        ClientNotification note;
        note.kind = NotificationKind::DeviceError;
        note.error = makeError(ErrorDomain::Service, ClientErrorCode::Timeout, "late");
        queue.push(std::move(note));
    }

    CHECK(queue.size() == 16U);
    CHECK(queue.dropped() == 84U);

    ClientNotification note;
    REQUIRE(queue.tryPop(note));
    CHECK(note.kind == NotificationKind::DeviceError);
    REQUIRE(note.error.has_value());
    CHECK(static_cast<ClientErrorCode>(note.error->code) == ClientErrorCode::Timeout);
}
