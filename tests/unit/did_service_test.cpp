#include <catch2/catch_all.hpp>

#include "uwb/protocol/capabilities.hpp"
#include "uwb/protocol/did_records.hpp"
#include "uwb/protocol/dids.hpp"
#include "uwb/protocol/uuid.hpp"
#include "uwb/server/connection_context.hpp"
#include "uwb/server/did_service.hpp"
#include "uwb/server/server_config.hpp"
#include "uwb/server/server_errors.hpp"

#include "server_fakes.hpp"
#include "test_helpers.hpp"

using namespace uwb::server;
using namespace uwb::test;
using uwb::protocol::ByteBuffer;
using uwb::protocol::Capability;
using uwb::protocol::ConstBytes;
using uwb::protocol::Did;
using uwb::protocol::ServiceNrc;
using uwb::protocol::TimeoutConfigRecord;
using uwb::protocol::Uuid;

namespace {

constexpr std::array<std::uint8_t, 8> kBoardId{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};

struct Fixture {
    ServerConfig config{};
    FakeStorage storage;
    FakeClock clock{1'000'000};
    DidService dids{config, storage};

    Fixture() {
        auto uuid = uwb::protocol::deviceUuidFromBoardId(uwb::protocol::ConstBytes{kBoardId.data(), kBoardId.size()});
        REQUIRE(uuid.ok());
        dids.setDeviceIdentity(uuid.value(), kBoardId, "pico-anchor", 0x1000, 45678);
    }
};

ConnectionContext controlConnection(ConnectionId id = 1) {
    ConnectionContext ctx;
    ctx.id = id;
    ctx.state = ConnectionState::Active;
    ctx.role = ConnectionRole::Control;
    ctx.session = SessionState::Extended;
    return ctx;
}

ConnectionContext observerConnection(ConnectionId id = 2) {
    ConnectionContext ctx = controlConnection(id);
    ctx.role = ConnectionRole::Observer;
    ctx.session = SessionState::Default;
    return ctx;
}

} // namespace

TEST_CASE("core-owned DIDs are answered by the server core", "[unit][server][did]") {
    Fixture f;

    const auto uuid = f.dids.read(Did::DeviceUuid);
    REQUIRE(uuid.ok());
    CHECK(uuid.value.size() == 16);

    const auto board = f.dids.read(Did::PicoBoardUniqueId);
    REQUIRE(board.ok());
    CHECK(sameBytes(uwb::protocol::ConstBytes{board.value.data(), board.value.size()},
                    uwb::protocol::ConstBytes{kBoardId.data(), kBoardId.size()}));

    const auto address = f.dids.read(Did::LogicalAddress);
    REQUIRE(address.ok());
    auto decoded = uwb::protocol::decodeU16Record(uwb::protocol::ConstBytes{address.value.data(),
                                                                            address.value.size()});
    REQUIRE(decoded.ok());
    CHECK(decoded.value() == 0x1000);
}

TEST_CASE("UWB-backed DIDs report NeedsBackend for asynchronous handling", "[unit][server][did]") {
    Fixture f;

    const auto result = f.dids.read(Did::UwbDeviceParameters);
    CHECK(result.status == ServerStatus::NeedsBackend);
    CHECK(!nrcFor(result.status).has_value()); // asynchronous path, not a negative response

    const auto complete = f.dids.read(Did::UwbCompleteConfiguration);
    CHECK(complete.status == ServerStatus::NeedsBackend);
}

TEST_CASE("unknown DID is answered with RequestOutOfRange", "[unit][server][did]") {
    Fixture f;
    const auto result = f.dids.read(static_cast<Did>(0xF123));
    CHECK(result.status == ServerStatus::UnknownDid);
    CHECK(nrcFor(result.status) == ServiceNrc::RequestOutOfRange);
}

TEST_CASE("write policy: Control + Extended Session is required", "[unit][server][did][policy]") {
    Fixture f;
    const ByteBuffer record = uwb::protocol::encodeRecord(TimeoutConfigRecord{1000, 100, 5000, 60000, 10000});

    const auto byObserver = f.dids.write(2, observerConnection(), Did::TimeoutConfiguration,
                                         uwb::protocol::bytesOf(record));
    CHECK(byObserver.status == ServerStatus::RoleDenied); // §9.2
    CHECK(nrcFor(byObserver.status) == ServiceNrc::ConditionsNotCorrect);

    ConnectionContext defaultSession = controlConnection();
    defaultSession.session = SessionState::Default;
    const auto byDefaultSession = f.dids.write(1, defaultSession, Did::TimeoutConfiguration,
                                               uwb::protocol::bytesOf(record));
    CHECK(byDefaultSession.status == ServerStatus::SessionDenied); // §22.3

    const auto byControl = f.dids.write(1, controlConnection(), Did::TimeoutConfiguration,
                                        uwb::protocol::bytesOf(record));
    CHECK(byControl.ok());
    CHECK(f.storage.stageCalls_ == 1);
    CHECK(f.storage.commitCalls_ == 1);
}

TEST_CASE("security-protected device requires an unlocked connection", "[unit][server][did][policy]") {
    Fixture f;
    f.config.securityEnabled = true;
    const ByteBuffer record = uwb::protocol::encodeRecord(TimeoutConfigRecord{1000, 100, 5000, 60000, 10000});

    const auto locked = f.dids.write(1, controlConnection(), Did::TimeoutConfiguration,
                                     uwb::protocol::bytesOf(record));
    CHECK(locked.status == ServerStatus::SecurityDenied);
    CHECK(nrcFor(locked.status) == ServiceNrc::SecurityAccessDenied);

    ConnectionContext unlocked = controlConnection();
    unlocked.security = SecurityState::Unlocked;
    CHECK(f.dids.write(1, unlocked, Did::TimeoutConfiguration, uwb::protocol::bytesOf(record)).ok());
}

TEST_CASE("read-only DIDs reject writes", "[unit][server][did][policy]") {
    Fixture f;
    ConnectionContext unlocked = controlConnection();
    unlocked.security = SecurityState::Unlocked;

    const auto result = f.dids.write(1, unlocked, Did::DeviceUuid, ByteBuffer(16, 0xAA));
    CHECK(result.status == ServerStatus::ReadOnlyDid);
    CHECK(f.storage.stageCalls_ == 0);
}

TEST_CASE("record validation and §52 bounds are enforced on write", "[unit][server][did][validation]") {
    Fixture f;
    ConnectionContext unlocked = controlConnection();
    unlocked.security = SecurityState::Unlocked;

    const ByteBuffer wrongLength(4, 0x01);
    const auto tooShort = f.dids.write(1, unlocked, Did::TimeoutConfiguration, uwb::protocol::bytesOf(wrongLength));
    CHECK(tooShort.status == ServerStatus::InvalidRecord);
    CHECK(nrcFor(tooShort.status) == ServiceNrc::IncorrectMessageLengthOrInvalidFormat);

    const ByteBuffer outOfBounds = uwb::protocol::encodeRecord(TimeoutConfigRecord{0, 100, 5000, 60000, 10000});
    const auto badTimeout = f.dids.write(1, unlocked, Did::TimeoutConfiguration, uwb::protocol::bytesOf(outOfBounds));
    CHECK(badTimeout.status == ServerStatus::InvalidAddress); // §52 range -> NRC 0x31
    CHECK(nrcFor(badTimeout.status) == ServiceNrc::RequestOutOfRange);

    const ByteBuffer badOverride = uwb::protocol::encodeRecord(
        uwb::protocol::CapabilityOverrideRecord{static_cast<uwb::protocol::CapabilityMask>(Capability::RawAt),
                                                static_cast<uwb::protocol::CapabilityMask>(Capability::RawAt)});
    const auto overrideClash = f.dids.write(1, unlocked, Did::CapabilityOverride, uwb::protocol::bytesOf(badOverride));
    CHECK(overrideClash.status == ServerStatus::InvalidAddress); // §40.8

    // A client address outside the device range must not be accepted (§40.4).
    const ByteBuffer clientAddress = uwb::protocol::encodeU16Record(0x0E10);
    const auto badAddress = f.dids.write(1, unlocked, Did::LogicalAddress, uwb::protocol::bytesOf(clientAddress));
    CHECK(badAddress.status == ServerStatus::InvalidAddress);
}

TEST_CASE("a failed storage commit rolls the DID value back", "[unit][server][did][atomic]") {
    Fixture f;
    ConnectionContext unlocked = controlConnection();
    unlocked.security = SecurityState::Unlocked;

    const ByteBuffer good = uwb::protocol::encodeRecord(TimeoutConfigRecord{1000, 100, 5000, 60000, 10000});
    REQUIRE(f.dids.write(1, unlocked, Did::TimeoutConfiguration, uwb::protocol::bytesOf(good)).ok());

    const auto before = f.dids.read(Did::TimeoutConfiguration);
    REQUIRE(before.ok());

    f.storage.commitSucceeds_ = false;
    const ByteBuffer candidate = uwb::protocol::encodeRecord(TimeoutConfigRecord{2000, 200, 8000, 30000, 20000});
    const auto failed = f.dids.write(1, unlocked, Did::TimeoutConfiguration, uwb::protocol::bytesOf(candidate));

    CHECK(failed.status == ServerStatus::StorageUnavailable);
    CHECK(f.storage.discardCalls_ == 1);
    CHECK(f.dids.timeouts().uartCommandTimeoutMs == 1000); // previous value still in effect (§27)

    const auto after = f.dids.read(Did::TimeoutConfiguration);
    REQUIRE(after.ok());
    CHECK(sameBytes(uwb::protocol::ConstBytes{before.value.data(), before.value.size()},
                    uwb::protocol::ConstBytes{after.value.data(), after.value.size()}));
}

TEST_CASE("effective capabilities apply the override record", "[unit][server][did][capabilities]") {
    Fixture f;
    ConnectionContext unlocked = controlConnection();
    unlocked.security = SecurityState::Unlocked;

    const uwb::protocol::CapabilityMask detected = Capability::Range | Capability::PdoaAzimuth;
    f.dids.setCapabilityMasks(uwb::protocol::kKnownCapabilityMask, detected);

    auto effective = f.dids.read(Did::EffectiveCapabilities);
    REQUIRE(effective.ok());
    auto mask = uwb::protocol::decodeU64Record(uwb::protocol::ConstBytes{effective.value.data(),
                                                                        effective.value.size()});
    REQUIRE(mask.ok());
    CHECK(mask.value() == detected);

    const ByteBuffer overrideRecord = uwb::protocol::encodeRecord(uwb::protocol::CapabilityOverrideRecord{
        static_cast<uwb::protocol::CapabilityMask>(Capability::SecurityAccess),
        static_cast<uwb::protocol::CapabilityMask>(Capability::PdoaAzimuth)});
    REQUIRE(f.dids.write(1, unlocked, Did::CapabilityOverride, uwb::protocol::bytesOf(overrideRecord)).ok());

    effective = f.dids.read(Did::EffectiveCapabilities);
    REQUIRE(effective.ok());
    mask = uwb::protocol::decodeU64Record(uwb::protocol::ConstBytes{effective.value.data(), effective.value.size()});
    REQUIRE(mask.ok());
    CHECK(uwb::protocol::hasCapability(mask.value(), Capability::SecurityAccess));  // forced on
    CHECK_FALSE(uwb::protocol::hasCapability(mask.value(), Capability::PdoaAzimuth)); // forced off
}

TEST_CASE("timeout settings round trip through the 0xF009 record", "[unit][server][did]") {
    TimeoutSettings timeouts;
    timeouts.uartCommandTimeoutMs = 1500;
    timeouts.sessionInactivityTimeoutMs = 7000;

    const auto record = toTimeoutRecord(timeouts);
    const auto back = fromTimeoutRecord(record);
    CHECK(back.uartCommandTimeoutMs == 1500);
    CHECK(back.sessionInactivityTimeoutMs == 7000);

    CHECK(validateTimeouts(timeouts).ok());

    TimeoutSettings outOfBounds;
    outOfBounds.uartCommandTimeoutMs = 0;
    const auto invalid = validateTimeouts(outOfBounds);
    CHECK_FALSE(invalid.ok());
}

TEST_CASE("runtime DIDs mirror server-core state", "[unit][server][did]") {
    Fixture f;
    f.dids.setUptime(1234567ULL);
    f.dids.setWifiRssi(-57);
    f.dids.setLatestDistance(2345);

    auto uptime = f.dids.read(Did::Uptime);
    REQUIRE(uptime.ok());
    auto uptimeValue = uwb::protocol::decodeU64Record(uwb::protocol::ConstBytes{uptime.value.data(),
                                                                                uptime.value.size()});
    REQUIRE(uptimeValue.ok());
    CHECK(uptimeValue.value() == 1234567ULL);

    auto rssi = f.dids.read(Did::WifiRssi);
    REQUIRE(rssi.ok());
    auto rssiValue = uwb::protocol::decodeI16Record(uwb::protocol::ConstBytes{rssi.value.data(), rssi.value.size()});
    REQUIRE(rssiValue.ok());
    CHECK(rssiValue.value() == -57);

    auto distance = f.dids.read(Did::UwbLatestDistance);
    REQUIRE(distance.ok());
    auto distanceValue = uwb::protocol::decodeI32Record(uwb::protocol::ConstBytes{distance.value.data(),
                                                                                  distance.value.size()});
    REQUIRE(distanceValue.ok());
    CHECK(distanceValue.value() == 2345);
}
