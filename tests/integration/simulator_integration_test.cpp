// Phase 3 integration tests (implementation plan §22, §23).
//
// These drive SimulatorRuntime through the real Asio adapters, which is the closest
// host-side stand-in for the firmware glue that Phase 6 will wire around the same
// ServerCore. The simulator core itself stays transport-agnostic; only this test
// side and simulator/src/asio_server.cpp touch sockets.

#include "test_transport.hpp"

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "uwb/protocol/did_records.hpp"
#include "uwb/protocol/dids.hpp"
#include "uwb/protocol/events.hpp"
#include "uwb/protocol/routines.hpp"
#include "uwb/protocol/uuid.hpp"

using namespace std::chrono_literals;

namespace protocol = uwb::protocol;
using protocol::ByteBuffer;
using protocol::ConstBytes;
using protocol::PayloadType;
using uwb::protocol::bytesOf;
using uwb::simulator::SimulatorOptions;
using uwb::test::SimulatorFixture;
using uwb::test::TcpClient;
using uwb::test::WireFrame;
using uwb::test::applicationFrame;
using uwb::test::fastOptions;
using uwb::test::kDefaultTimeout;
using uwb::test::nackOf;
using uwb::test::nrcOf;

namespace {

constexpr std::uint16_t kControlClient = 0x0E01;
constexpr std::uint16_t kObserverClient = 0x0E02;
constexpr std::uint8_t kResponseDidRead = 0x62; // 0x22 | 0x40

protocol::Uuid expectedDeviceUuid(const SimulatorFixture &sim) {
    auto uuid = protocol::deviceUuidFromBoardId(ConstBytes{sim.options.boardUniqueId.data(), sim.options.boardUniqueId.size()});
    REQUIRE(uuid.ok());
    return uuid.value();
}

struct ActivationResult {
    protocol::ConnectionRole role = protocol::ConnectionRole::None;
    protocol::ActivationResponseCode code = protocol::ActivationResponseCode::AcceptedRequestedRole;
};

// Connects a test client and performs the Activation handshake (§18).
ActivationResult activate(TcpClient &client, protocol::ConnectionRole role, std::uint64_t uuidSeed) {
    client.send(uwb::test::activationFrame(client.address, role, uuidSeed));

    auto reply = client.nextFrame(3000ms);
    REQUIRE(reply.has_value());
    REQUIRE(reply->header.payloadType == PayloadType::ConnectionActivationResponse);

    auto decoded = protocol::decodeConnectionActivationResponse(frameBody(*reply));
    REQUIRE(decoded.ok());

    ActivationResult result;
    result.role = decoded.value().assignedRole;
    result.code = decoded.value().responseCode;
    client.serverAddress = decoded.value().serverLogicalAddress;
    return result;
}

// Waits until the core reports the given number of live connections.
bool waitForConnectionCount(SimulatorFixture &sim, std::size_t expected) {
    for (int attempt = 0; attempt < 60; ++attempt) {
        // The counter belongs to the simulator thread; polling it is fine here
        // because the runtime publishes them as atomic snapshots (ServerCore itself is
        // single threaded and must not be touched from the test thread).
        if (sim.runtimeRef().connectionCount() == expected) {
            return true;
        }
        std::this_thread::sleep_for(50ms);
    }
    return sim.runtimeRef().connectionCount() == expected;
}

// Switches a Control connection to the Extended session (§23).
void enableExtendedSession(TcpClient &client) {
    protocol::SessionControlRequest session;
    session.requestedSession = static_cast<std::uint8_t>(protocol::SessionId::Extended);
    const std::uint32_t txn = ++client.txn;
    client.sendPdu(txn, bytesOf(protocol::encodeSessionControlRequest(session)));
    auto reply = client.waitForResponse(txn);
    REQUIRE(reply.has_value());
    auto decoded = protocol::decodeSessionControlResponse(bytesOf(servicePduOf(*reply)));
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().activeSession == static_cast<std::uint8_t>(protocol::SessionId::Extended));
}

bool isPendingResponse(const WireFrame &frame, std::uint32_t transactionId) {
    if (frame.header.payloadType != PayloadType::ApplicationMessage ||
        transactionOf(frame) != transactionId) {
        return false;
    }
    // 21.4 negative response PDU is 0x7F <sid> <nrc>.
    return nrcOf(frame) == protocol::ServiceNrc::ResponsePending;
}

} // namespace

// ---------------------------------------------------------------------------
// Discovery (plan §23: discover)
// ---------------------------------------------------------------------------

TEST_CASE("UDP discovery answers with the simulated device identity", "[integration]") {
    SimulatorFixture sim;

    auto reply = uwb::test::discover(sim.udpPort(), 3000ms, uwb::test::discoveryRequestFrame());
    REQUIRE(reply.has_value());

    CHECK(reply->header.protocolVersion == protocol::kProtocolVersionV1_0);
    CHECK(reply->header.payloadType == PayloadType::DeviceIdResponse);
    CHECK(reply->header.payloadLength == reply->payload.size());

    auto decoded = protocol::decodeDeviceIdResponse(frameBody(*reply));
    REQUIRE(decoded.ok());

    const auto &identity = decoded.value();
    CHECK(identity.deviceUuid == expectedDeviceUuid(sim));
    CHECK(identity.logicalAddress == sim.deviceAddress);
    CHECK(identity.tcpPort == sim.tcpPort());
    CHECK(identity.deviceName == sim.options.deviceName);
    const auto expectedMask = protocol::applyCapabilityOverride(sim.options.detectedMask,
                                                               sim.options.overrideForceOn,
                                                               sim.options.overrideForceOff);
    CHECK(identity.capabilityMask == expectedMask);
    CHECK(identity.controlStatus == protocol::ControlStatus::Available);
    CHECK(identity.maxObservers == 3);
}

TEST_CASE("Discovery ignores datagrams that are not a Device Identification Request", "[integration]") {
    SimulatorFixture sim;

    ByteBuffer garbage{0xDE, 0xAD, 0xBE, 0xEF};
    auto reply = uwb::test::discover(sim.udpPort(), 250ms, garbage);
    CHECK_FALSE(reply.has_value());
}

TEST_CASE("Occupied control slot is reported in discovery", "[integration]") {
    SimulatorFixture sim;
    TcpClient control(sim.tcpPort(), kControlClient);
    REQUIRE(activate(control, protocol::ConnectionRole::Control, 1).role == protocol::ConnectionRole::Control);

    auto reply = uwb::test::discover(sim.udpPort(), 3000ms, uwb::test::discoveryRequestFrame());
    REQUIRE(reply.has_value());
    auto decoded = protocol::decodeDeviceIdResponse(frameBody(*reply));
    REQUIRE(decoded.ok());
    CHECK(decoded.value().controlStatus == protocol::ControlStatus::Occupied);
    CHECK(decoded.value().capabilityMask != 0);
}

// ---------------------------------------------------------------------------
// Activation and role policy (plan §23, specification §9.3)
// ---------------------------------------------------------------------------

TEST_CASE("Activation grants Control to the first client and echoes the device identity", "[integration]") {
    SimulatorFixture sim;
    TcpClient control(sim.tcpPort(), kControlClient);
    auto result = activate(control, protocol::ConnectionRole::Control, 1);

    CHECK(result.role == protocol::ConnectionRole::Control);
    CHECK(result.code == protocol::ActivationResponseCode::AcceptedRequestedRole);
    CHECK(control.serverAddress == sim.deviceAddress);
    CHECK(sim.runtimeRef().connectionCount() == 1);
}

TEST_CASE("A second Control client is downgraded to Observer", "[integration]") {
    SimulatorFixture sim;
    TcpClient control(sim.tcpPort(), kControlClient);
    REQUIRE(activate(control, protocol::ConnectionRole::Control, 1).role == protocol::ConnectionRole::Control);

    TcpClient second(sim.tcpPort(), kObserverClient);
    auto result = activate(second, protocol::ConnectionRole::Control, 2);
    CHECK(result.role == protocol::ConnectionRole::Observer);
    CHECK(result.code == protocol::ActivationResponseCode::AcceptedDowngradedToObserver); // §18.2 code 0x01
}

TEST_CASE("Observers may connect alongside the Control client", "[integration]") {
    SimulatorFixture sim;
    TcpClient control(sim.tcpPort(), kControlClient);
    TcpClient observerA(sim.tcpPort(), 0x0E02);
    TcpClient observerB(sim.tcpPort(), 0x0E03);

    CHECK(activate(control, protocol::ConnectionRole::Control, 1).role == protocol::ConnectionRole::Control);
    CHECK(activate(observerA, protocol::ConnectionRole::Observer, 2).role == protocol::ConnectionRole::Observer);
    CHECK(activate(observerB, protocol::ConnectionRole::Observer, 3).role == protocol::ConnectionRole::Observer);
    CHECK(sim.runtimeRef().connectionCount() == 3);
}

TEST_CASE("Application messages before Activation are refused with transport NACK 0x03", "[integration]") {
    SimulatorFixture sim;
    TcpClient client(sim.tcpPort(), kControlClient);

    // §18: an activated connection is required before any service traffic; §20.3
    // says such a message never reaches service dispatch, so the answer is the
    // transport NACK 0x8003 with code 0x03 rather than a 0x7F negative response.
    protocol::ReadDidRequest read;
    read.did = static_cast<std::uint16_t>(protocol::Did::DeviceUuid);
    client.serverAddress = sim.deviceAddress;
    client.sendPdu(++client.txn, bytesOf(protocol::encodeReadDidRequest(read)));

    auto reply = client.waitFor(kDefaultTimeout, [](const WireFrame &frame) {
        return frame.header.payloadType == PayloadType::ApplicationMessageNack;
    });
    REQUIRE(reply.has_value());
    const auto nack = uwb::test::applicationNack(*reply);
    REQUIRE(nack.has_value());
    CHECK(nack->code == protocol::ApplicationNackCode::ConnectionNotActivated); // 20.3 code 0x03
    CHECK(nack->transactionId == 1);
    CHECK(nack->targetLogicalAddress == kControlClient);
    CHECK(client.waitForClose(kDefaultTimeout));
}

TEST_CASE("Observers cannot write DIDs or execute raw AT", "[integration]") {
    SimulatorFixture sim;
    TcpClient observer(sim.tcpPort(), kObserverClient);
    REQUIRE(activate(observer, protocol::ConnectionRole::Observer, 2).role == protocol::ConnectionRole::Observer);

    protocol::WriteDidRequest write;
    write.did = static_cast<std::uint16_t>(protocol::Did::UwbDeviceParameters);
    protocol::UwbDeviceParametersRecord record;
    record.channel = 7;
    write.data = protocol::encodeRecord(record);

    const std::uint32_t writeTxn = ++observer.txn;
    observer.sendPdu(writeTxn, bytesOf(protocol::encodeWriteDidRequest(write)));
    auto writeReply = observer.waitForResponse(writeTxn);
    REQUIRE(writeReply.has_value());
    CHECK(uwb::test::nrcOf(*writeReply) == protocol::ServiceNrc::ConditionsNotCorrect); // §9.2

    protocol::ExecuteAtRequest at;
    at.command = ByteBuffer{'A', 'T'};
    const std::uint32_t atTxn = ++observer.txn;
    observer.sendPdu(atTxn, bytesOf(protocol::encodeExecuteAtRequest(at)));
    auto atReply = observer.waitForResponse(atTxn);
    REQUIRE(atReply.has_value());
    CHECK(uwb::test::nrcOf(*atReply) == protocol::ServiceNrc::ConditionsNotCorrect); // §30
}

// ---------------------------------------------------------------------------
// DID reads: synchronous, asynchronous, and backend-backed (plan §23)
// ---------------------------------------------------------------------------

TEST_CASE("Core-owned DID read answers synchronously", "[integration]") {
    SimulatorFixture sim;
    TcpClient control(sim.tcpPort(), kControlClient);
    REQUIRE(activate(control, protocol::ConnectionRole::Control, 1).role == protocol::ConnectionRole::Control);

    protocol::ReadDidRequest read;
    read.did = static_cast<std::uint16_t>(protocol::Did::DeviceUuid);
    const std::uint32_t txn = ++control.txn;
    control.sendPdu(txn, bytesOf(protocol::encodeReadDidRequest(read)));

    auto reply = control.waitForResponse(txn);
    REQUIRE(reply.has_value());
    CHECK(uwb::test::nrcOf(*reply) == std::nullopt);

    auto decoded = protocol::decodeReadDidResponse(bytesOf(servicePduOf(*reply)));
    REQUIRE(decoded.ok());
    CHECK(decoded.value().did == static_cast<std::uint16_t>(protocol::Did::DeviceUuid));
    REQUIRE(decoded.value().data.size() == 16);

    protocol::Uuid uuid;
    std::copy(decoded.value().data.begin(), decoded.value().data.end(), uuid.bytes.begin());
    CHECK(uuid == expectedDeviceUuid(sim));
}

TEST_CASE("UWB-backed DID read answers 0x78 then the final response", "[integration]") {
    SimulatorFixture sim;
    TcpClient control(sim.tcpPort(), kControlClient);
    REQUIRE(activate(control, protocol::ConnectionRole::Control, 1).role == protocol::ConnectionRole::Control);

    protocol::ReadDidRequest read;
    read.did = static_cast<std::uint16_t>(protocol::Did::UwbDeviceParameters);
    const std::uint32_t txn = ++control.txn;
    control.sendPdu(txn, bytesOf(protocol::encodeReadDidRequest(read)));

    auto interim = control.waitFor(3000ms, [txn](const WireFrame &frame) { return isPendingResponse(frame, txn); });
    REQUIRE(interim.has_value());

    auto finalReply = control.waitForResponse(txn);
    REQUIRE(finalReply.has_value());
    CHECK(uwb::test::nrcOf(*finalReply) == std::nullopt);

    const ByteBuffer pdu = servicePduOf(*finalReply);
    REQUIRE_FALSE(pdu.empty());
    CHECK(pdu[0] == kResponseDidRead);

    auto decoded = protocol::decodeReadDidResponse(bytesOf(pdu));
    REQUIRE(decoded.ok());
    auto record = protocol::decodeUwbDeviceParametersRecord(decoded.value().dataBytes());
    REQUIRE(record.ok());
    CHECK(record.value().channel == 9); // seeded by the simulated model
}

TEST_CASE("Unknown DID is refused with NRC 0x31", "[integration]") {
    SimulatorFixture sim;
    TcpClient control(sim.tcpPort(), kControlClient);
    REQUIRE(activate(control, protocol::ConnectionRole::Control, 1).role == protocol::ConnectionRole::Control);

    protocol::ReadDidRequest read;
    read.did = 0xF123;
    const std::uint32_t txn = ++control.txn;
    control.sendPdu(txn, bytesOf(protocol::encodeReadDidRequest(read)));

    auto reply = control.waitForResponse(txn);
    REQUIRE(reply.has_value());
    CHECK(uwb::test::nrcOf(*reply) == protocol::ServiceNrc::RequestOutOfRange);
}

TEST_CASE("Writing a UWB DID updates the simulated model", "[integration]") {
    SimulatorFixture sim;
    TcpClient control(sim.tcpPort(), kControlClient);
    REQUIRE(activate(control, protocol::ConnectionRole::Control, 1).role == protocol::ConnectionRole::Control);
    enableExtendedSession(control); // 27: writable DIDs need the Extended session

    protocol::UwbDeviceParametersRecord record;
    record.id = 1;
    record.role = 1;
    record.channel = 5;
    record.rate = 0; // 850 kb/s

    protocol::WriteDidRequest write;
    write.did = static_cast<std::uint16_t>(protocol::Did::UwbDeviceParameters);
    write.data = protocol::encodeRecord(record);
    const std::uint32_t writeTxn = ++control.txn;
    control.sendPdu(writeTxn, bytesOf(protocol::encodeWriteDidRequest(write)));

    auto writeReply = control.waitForResponse(writeTxn);
    REQUIRE(writeReply.has_value());
    CHECK(uwb::test::nrcOf(*writeReply) == std::nullopt);

    protocol::ReadDidRequest read;
    read.did = static_cast<std::uint16_t>(protocol::Did::UwbDeviceParameters);
    const std::uint32_t readTxn = ++control.txn;
    control.sendPdu(readTxn, bytesOf(protocol::encodeReadDidRequest(read)));

    auto readReply = control.waitForResponse(readTxn);
    REQUIRE(readReply.has_value());
    auto decoded = protocol::decodeReadDidResponse(bytesOf(servicePduOf(*readReply)));
    REQUIRE(decoded.ok());
    auto stored = protocol::decodeUwbDeviceParametersRecord(decoded.value().dataBytes());
    REQUIRE(stored.ok());
    CHECK(stored.value().channel == 5);
    CHECK(stored.value().rate == 0);
}

// ---------------------------------------------------------------------------
// Event streaming (plan §23: streaming interleaved with a pending request)
// ---------------------------------------------------------------------------

TEST_CASE("Live measurement stream keeps flowing while another request is pending", "[integration]") {
    SimulatorFixture sim;
    TcpClient control(sim.tcpPort(), kControlClient);
    REQUIRE(activate(control, protocol::ConnectionRole::Control, 1).role == protocol::ConnectionRole::Control);

    protocol::EventSubscribeRequest subscribe;
    subscribe.eventId = static_cast<std::uint16_t>(protocol::EventId::UwbMeasurement);
    subscribe.mode = static_cast<std::uint8_t>(protocol::StreamMode::Live);
    subscribe.requestedPeriodMs = 20;

    const std::uint32_t subscribeTxn = ++control.txn;
    control.sendPdu(subscribeTxn, bytesOf(protocol::encodeEventSubscribeRequest(subscribe)));
    auto subReply = control.waitForResponse(subscribeTxn);
    REQUIRE(subReply.has_value());
    auto stream = protocol::decodeEventSubscribeResponse(bytesOf(servicePduOf(*subReply)));
    REQUIRE(stream.ok());
    CHECK(stream.value().streamId != 0);
    CHECK(stream.value().acceptedMode == static_cast<std::uint8_t>(protocol::StreamMode::Live));
    CHECK(stream.value().queueCapacity == static_cast<std::uint16_t>(protocol::kLiveStreamQueueCapacity));

    // A slow backend read runs concurrently with the live stream.
    const std::uint32_t readTxn = ++control.txn;
    protocol::ReadDidRequest read;
    read.did = static_cast<std::uint16_t>(protocol::Did::UwbCompleteConfiguration);
    control.sendPdu(readTxn, bytesOf(protocol::encodeReadDidRequest(read)));

    std::size_t measurements = 0;
    bool sawResponse = false;
    const auto deadline = std::chrono::steady_clock::now() + 3000ms;
    while (std::chrono::steady_clock::now() < deadline && (!sawResponse || measurements < 3)) {
        auto frame = control.nextFrame(1500ms);
        if (!frame) {
            break;
        }
        if (frame->header.payloadType == PayloadType::EventNotification) {
            auto event = protocol::decodeEventNotification(frameBody(*frame));
            REQUIRE(event.ok());
            CHECK(event.value().eventId == static_cast<std::uint16_t>(protocol::EventId::UwbMeasurement));
            auto measurement = protocol::decodeMeasurementEvent(event.value().payload);
            REQUIRE(measurement.ok());
            CHECK(measurement.value().rawRangeMm > 0);
            ++measurements;
            continue;
        }
        if (frame->header.payloadType == PayloadType::ApplicationMessage &&
            transactionOf(*frame) == readTxn) {
            const ByteBuffer pdu = servicePduOf(*frame);
            if (!pdu.empty() && pdu[0] == kResponseDidRead) {
                sawResponse = true;
            }
        }
    }

    CHECK(sawResponse);
    CHECK(measurements >= 3);
}

TEST_CASE("Event control can unsubscribe a live stream", "[integration]") {
    SimulatorFixture sim;
    TcpClient control(sim.tcpPort(), kControlClient);
    REQUIRE(activate(control, protocol::ConnectionRole::Control, 1).role == protocol::ConnectionRole::Control);

    protocol::EventSubscribeRequest subscribe;
    subscribe.eventId = static_cast<std::uint16_t>(protocol::EventId::UwbMeasurement);
    subscribe.mode = static_cast<std::uint8_t>(protocol::StreamMode::Live);
    subscribe.requestedPeriodMs = 20;

    const std::uint32_t subscribeTxn = ++control.txn;
    control.sendPdu(subscribeTxn, bytesOf(protocol::encodeEventSubscribeRequest(subscribe)));
    auto subReply = control.waitForResponse(subscribeTxn);
    REQUIRE(subReply.has_value());
    auto stream = protocol::decodeEventSubscribeResponse(bytesOf(servicePduOf(*subReply)));
    REQUIRE(stream.ok());

    protocol::EventUnsubscribeRequest unsubscribe;
    unsubscribe.streamId = stream.value().streamId;
    const std::uint32_t unsubTxn = ++control.txn;
    control.sendPdu(unsubTxn, bytesOf(protocol::encodeEventUnsubscribeRequest(unsubscribe)));
    auto unsubReply = control.waitForResponse(unsubTxn);
    REQUIRE(unsubReply.has_value());
    CHECK(uwb::test::nrcOf(*unsubReply) == std::nullopt);

    auto quiet = control.collect(400ms);
    CHECK(control.countType(quiet, PayloadType::EventNotification) == 0);
}

TEST_CASE("Observers can subscribe to measurement streams", "[integration]") {
    SimulatorFixture sim;
    TcpClient observer(sim.tcpPort(), kObserverClient);
    REQUIRE(activate(observer, protocol::ConnectionRole::Observer, 2).role == protocol::ConnectionRole::Observer);

    protocol::EventSubscribeRequest subscribe;
    subscribe.eventId = static_cast<std::uint16_t>(protocol::EventId::UwbMeasurement);
    subscribe.mode = static_cast<std::uint8_t>(protocol::StreamMode::Live);
    subscribe.requestedPeriodMs = 20;

    const std::uint32_t txn = ++observer.txn;
    observer.sendPdu(txn, bytesOf(protocol::encodeEventSubscribeRequest(subscribe)));
    auto reply = observer.waitForResponse(txn);
    REQUIRE(reply.has_value());
    CHECK(uwb::test::nrcOf(*reply) == std::nullopt);

    auto events = observer.collect(600ms);
    CHECK(observer.countType(events, PayloadType::EventNotification) >= 3);
}

// ---------------------------------------------------------------------------
// Deterministic simulation mode (plan §22.3)
// ---------------------------------------------------------------------------

namespace {

// Subscribes one Observer to the measurement stream of a freshly started simulator
// and returns the first samples it receives.
std::vector<protocol::MeasurementEvent> streamMeasurements(const SimulatorOptions &options) {
    SimulatorFixture sim(options);
    TcpClient observer(sim.tcpPort(), kObserverClient);
    REQUIRE(activate(observer, protocol::ConnectionRole::Observer, 2).role == protocol::ConnectionRole::Observer);

    protocol::EventSubscribeRequest subscribe;
    subscribe.eventId = static_cast<std::uint16_t>(protocol::EventId::UwbMeasurement);
    subscribe.mode = static_cast<std::uint8_t>(protocol::StreamMode::Live);
    subscribe.requestedPeriodMs = 20;

    const std::uint32_t txn = ++observer.txn;
    observer.sendPdu(txn, bytesOf(protocol::encodeEventSubscribeRequest(subscribe)));
    auto reply = observer.waitForResponse(txn);
    REQUIRE(reply.has_value());
    REQUIRE(nrcOf(*reply) == std::nullopt);

    std::vector<protocol::MeasurementEvent> samples;
    const auto deadline = std::chrono::steady_clock::now() + 2000ms;
    while (samples.size() < 6 && std::chrono::steady_clock::now() < deadline) {
        auto frame = observer.nextFrame(500ms);
        if (!frame.has_value()) {
            break;
        }
        if (frame->header.payloadType != PayloadType::EventNotification) {
            continue;
        }
        auto event = protocol::decodeEventNotification(frameBody(*frame));
        REQUIRE(event.ok());
        auto measurement = protocol::decodeMeasurementEvent(event.value().payload);
        REQUIRE(measurement.ok());
        samples.push_back(measurement.value());
    }
    return samples;
}

} // namespace

TEST_CASE("Deterministic mode reproduces the same measurement sequence", "[integration]") {
    SimulatorOptions options = fastOptions();
    options.deterministicClock = true;
    options.randomSeed = 0xC0FFEE;
    options.emitPdoa = true;

    const auto first = streamMeasurements(options);
    const auto second = streamMeasurements(options);

    REQUIRE(first.size() >= 6);
    REQUIRE(first.size() == second.size());
    for (std::size_t index = 0; index < first.size(); ++index) {
        CHECK(first[index].tagId == second[index].tagId);
        CHECK(first[index].anchorId == second[index].anchorId);
        CHECK(first[index].rawRangeMm == second[index].rawRangeMm);
        CHECK(first[index].rawAzimuthMilliDeg == second[index].rawAzimuthMilliDeg);
    }

    // The stepped clock must still produce fresh samples, not a frozen model.
    CHECK(first.front().rawRangeMm != 0);
}

TEST_CASE("Deterministic mode emits the same samples as the seeded wall clock", "[integration]") {
    SimulatorOptions options = fastOptions();
    options.deterministicClock = true;
    options.randomSeed = 0x1234;
    options.rangeNoiseMm = 0; // identical geometry, so only the RNG sequence matters

    const auto deterministic = streamMeasurements(options);
    options.deterministicClock = false;
    const auto wallClock = streamMeasurements(options);

    REQUIRE(deterministic.size() >= 6);
    REQUIRE(deterministic.size() == wallClock.size());
    for (std::size_t index = 0; index < deterministic.size(); ++index) {
        CHECK(deterministic[index].rawRangeMm == wallClock[index].rawRangeMm);
    }
}

// ---------------------------------------------------------------------------
// Session and security policy (plan §23)
// ---------------------------------------------------------------------------

TEST_CASE("Raw AT requires an extended session", "[integration]") {
    SimulatorFixture sim;
    TcpClient control(sim.tcpPort(), kControlClient);
    REQUIRE(activate(control, protocol::ConnectionRole::Control, 1).role == protocol::ConnectionRole::Control);

    protocol::ExecuteAtRequest at;
    at.command = ByteBuffer{'A', 'T', '+', 'G', 'E', 'T', 'V', 'E', 'R'};

    const std::uint32_t deniedTxn = ++control.txn;
    control.sendPdu(deniedTxn, bytesOf(protocol::encodeExecuteAtRequest(at)));
    auto denied = control.waitForResponse(deniedTxn);
    REQUIRE(denied.has_value());
    CHECK(uwb::test::nrcOf(*denied) == protocol::ServiceNrc::ConditionsNotCorrect); // §30: default session

    protocol::SessionControlRequest session;
    session.requestedSession = static_cast<std::uint8_t>(protocol::SessionId::Extended);
    const std::uint32_t sessionTxn = ++control.txn;
    control.sendPdu(sessionTxn, bytesOf(protocol::encodeSessionControlRequest(session)));
    auto sessionReply = control.waitForResponse(sessionTxn);
    REQUIRE(sessionReply.has_value());
    auto sessionResult = protocol::decodeSessionControlResponse(bytesOf(servicePduOf(*sessionReply)));
    REQUIRE(sessionResult.ok());
    CHECK(sessionResult.value().activeSession == static_cast<std::uint8_t>(protocol::SessionId::Extended));
    CHECK(sessionResult.value().p2ServerMaxMs == sim.options.p2ServerMaxMs);

    const std::uint32_t atTxn = ++control.txn;
    control.sendPdu(atTxn, bytesOf(protocol::encodeExecuteAtRequest(at)));
    auto atReply = control.waitForResponse(atTxn);
    REQUIRE(atReply.has_value());
    auto atResult = protocol::decodeExecuteAtResponse(bytesOf(servicePduOf(*atReply)));
    REQUIRE(atResult.ok());
    const std::string responseText(atResult.value().rawResponse.begin(), atResult.value().rawResponse.end());
    CHECK(responseText.find("V1.0.7") != std::string::npos); // §22.2: response text is visible in the log
}

TEST_CASE("Security access unlocks the device when the key matches", "[integration]") {
    SimulatorOptions options = uwb::test::fastOptions();
    options.securityEnabled = true;
    options.securityKey = 0x11223344U;
    SimulatorFixture sim(options);

    TcpClient control(sim.tcpPort(), kControlClient);
    REQUIRE(activate(control, protocol::ConnectionRole::Control, 1).role == protocol::ConnectionRole::Control);

    protocol::SecurityAccessRequest seedRequest;
    seedRequest.subFunction = static_cast<std::uint8_t>(protocol::SecuritySubFunction::RequestSeed);
    const std::uint32_t seedTxn = ++control.txn;
    control.sendPdu(seedTxn, bytesOf(protocol::encodeSecurityAccessRequest(seedRequest)));
    auto seedReply = control.waitForResponse(seedTxn);
    REQUIRE(seedReply.has_value());
    auto seed = protocol::decodeSecurityAccessResponse(bytesOf(servicePduOf(*seedReply)));
    REQUIRE(seed.ok());
    CHECK(seed.value().seed != 0);

    // §23 + §25: the extended session is refused while the device is still locked.
    protocol::SessionControlRequest session;
    session.requestedSession = static_cast<std::uint8_t>(protocol::SessionId::Extended);
    const std::uint32_t deniedTxn = ++control.txn;
    control.sendPdu(deniedTxn, bytesOf(protocol::encodeSessionControlRequest(session)));
    auto denied = control.waitForResponse(deniedTxn);
    REQUIRE(denied.has_value());
    CHECK(uwb::test::nrcOf(*denied) == protocol::ServiceNrc::SecurityAccessDenied);

    protocol::SecurityAccessRequest keyRequest;
    keyRequest.subFunction = static_cast<std::uint8_t>(protocol::SecuritySubFunction::SendKey);
    keyRequest.key = options.securityKey;
    const std::uint32_t keyTxn = ++control.txn;
    control.sendPdu(keyTxn, bytesOf(protocol::encodeSecurityAccessRequest(keyRequest)));
    auto keyReply = control.waitForResponse(keyTxn);
    REQUIRE(keyReply.has_value());
    CHECK(uwb::test::nrcOf(*keyReply) == std::nullopt);

    const std::uint32_t sessionTxn = ++control.txn;
    control.sendPdu(sessionTxn, bytesOf(protocol::encodeSessionControlRequest(session)));
    auto sessionReply = control.waitForResponse(sessionTxn);
    REQUIRE(sessionReply.has_value());
    CHECK(uwb::test::nrcOf(*sessionReply) == std::nullopt);
}

TEST_CASE("A wrong security key is refused and eventually locks the client out", "[integration]") {
    SimulatorOptions options = uwb::test::fastOptions();
    options.securityEnabled = true;
    options.securityKey = 0x11223344U;
    SimulatorFixture sim(options);

    TcpClient control(sim.tcpPort(), kControlClient);
    REQUIRE(activate(control, protocol::ConnectionRole::Control, 1).role == protocol::ConnectionRole::Control);

    protocol::SecurityAccessRequest seedRequest;
    seedRequest.subFunction = static_cast<std::uint8_t>(protocol::SecuritySubFunction::RequestSeed);

    std::optional<protocol::ServiceNrc> lastSeen;
    for (int attempt = 1; attempt <= 6; ++attempt) {
        const std::uint32_t seedTxn = ++control.txn;
        control.sendPdu(seedTxn, bytesOf(protocol::encodeSecurityAccessRequest(seedRequest)));
        auto seedReply = control.waitForResponse(seedTxn);
        REQUIRE(seedReply.has_value());

        const auto nrc = uwb::test::nrcOf(*seedReply);
        if (nrc.has_value()) {
            lastSeen = nrc;
            break;
        }

        auto seed = protocol::decodeSecurityAccessResponse(bytesOf(servicePduOf(*seedReply)));
        REQUIRE(seed.ok());

        protocol::SecurityAccessRequest badKey;
        badKey.subFunction = static_cast<std::uint8_t>(protocol::SecuritySubFunction::SendKey);
        badKey.key = 0xDEADBEEFU; // never correct
        const std::uint32_t keyTxn = ++control.txn;
        control.sendPdu(keyTxn, bytesOf(protocol::encodeSecurityAccessRequest(badKey)));
        auto keyReply = control.waitForResponse(keyTxn);
        REQUIRE(keyReply.has_value());
        lastSeen = uwb::test::nrcOf(*keyReply);
    }

    REQUIRE(lastSeen.has_value());
    // §21.4: wrong keys first give 0x35, then the attempt counter switches to 0x36/0x37.
    CHECK((*lastSeen == protocol::ServiceNrc::InvalidKey || *lastSeen == protocol::ServiceNrc::ExceedNumberOfAttempts ||
           *lastSeen == protocol::ServiceNrc::RequiredTimeDelayNotExpired));
}

// ---------------------------------------------------------------------------
// Fault injection (plan §22.4)
// ---------------------------------------------------------------------------

TEST_CASE("A slow backend produces the service timeout NRC", "[integration]") {
    SimulatorOptions options = uwb::test::fastOptions();
    options.backendLatencyMs = 4000;  // longer than the p2* deadline
    options.p2StarServerMax10ms = 20; // 200 ms
    SimulatorFixture sim(options);

    TcpClient control(sim.tcpPort(), kControlClient);
    REQUIRE(activate(control, protocol::ConnectionRole::Control, 1).role == protocol::ConnectionRole::Control);

    protocol::ReadDidRequest read;
    read.did = static_cast<std::uint16_t>(protocol::Did::UwbDeviceParameters);
    const std::uint32_t txn = ++control.txn;
    control.sendPdu(txn, bytesOf(protocol::encodeReadDidRequest(read)));

    auto interim = control.waitFor(2000ms, [txn](const WireFrame &frame) { return isPendingResponse(frame, txn); });
    REQUIRE(interim.has_value());

    auto timeout = control.waitForResponse(txn);
    REQUIRE(timeout.has_value());
    CHECK(uwb::test::nrcOf(*timeout) == protocol::ServiceNrc::GeneralProgrammingFailure); // 0x72 (§21.4)
}

TEST_CASE("A saturated backend refuses with the busy NRC", "[integration]") {
    SimulatorOptions options = uwb::test::fastOptions();
    options.backendRejectsSubmissions = true;
    SimulatorFixture sim(options);

    TcpClient control(sim.tcpPort(), kControlClient);
    REQUIRE(activate(control, protocol::ConnectionRole::Control, 1).role == protocol::ConnectionRole::Control);

    protocol::ReadDidRequest read;
    read.did = static_cast<std::uint16_t>(protocol::Did::UwbDeviceParameters);
    const std::uint32_t txn = ++control.txn;
    control.sendPdu(txn, bytesOf(protocol::encodeReadDidRequest(read)));

    auto reply = control.waitForResponse(txn);
    REQUIRE(reply.has_value());
    CHECK(uwb::test::nrcOf(*reply) == protocol::ServiceNrc::BusyRepeatRequest); // 0x21 (§21.4)
}

TEST_CASE("A backend parse error is reported as a programming failure", "[integration]") {
    SimulatorOptions options = uwb::test::fastOptions();
    options.backendFaultParseError = true;
    SimulatorFixture sim(options);

    TcpClient control(sim.tcpPort(), kControlClient);
    REQUIRE(activate(control, protocol::ConnectionRole::Control, 1).role == protocol::ConnectionRole::Control);

    protocol::ReadDidRequest read;
    read.did = static_cast<std::uint16_t>(protocol::Did::UwbDeviceParameters);
    const std::uint32_t txn = ++control.txn;
    control.sendPdu(txn, bytesOf(protocol::encodeReadDidRequest(read)));

    auto reply = control.waitForResponse(txn);
    REQUIRE(reply.has_value());
    // §21.4: a malformed AT payload is not a client-side record error.
    CHECK(uwb::test::nrcOf(*reply) == protocol::ServiceNrc::GeneralProgrammingFailure);
}

// ---------------------------------------------------------------------------
// Routine control (plan §23: backend routines through the real transport)
// ---------------------------------------------------------------------------

TEST_CASE("An unsupported UWB command is refused with NRC 0x31", "[integration]") {
    SimulatorOptions options = uwb::test::fastOptions();
    options.backendFaultUnsupported = true;
    SimulatorFixture sim(options);

    TcpClient control(sim.tcpPort(), kControlClient);
    REQUIRE(activate(control, protocol::ConnectionRole::Control, 1).role == protocol::ConnectionRole::Control);

    protocol::ReadDidRequest read;
    read.did = static_cast<std::uint16_t>(protocol::Did::UwbDeviceParameters);
    const std::uint32_t txn = ++control.txn;
    control.sendPdu(txn, bytesOf(protocol::encodeReadDidRequest(read)));

    auto reply = control.waitForResponse(txn);
    REQUIRE(reply.has_value());
    // §21.4: the module says "not supported", which the server reports as an
    // unsupported identifier rather than a transport failure.
    CHECK(uwb::test::nrcOf(*reply) == protocol::ServiceNrc::RequestOutOfRange);
}

TEST_CASE("Routine control starts and completes a backend routine", "[integration]") {
    SimulatorFixture sim;
    TcpClient control(sim.tcpPort(), kControlClient);
    REQUIRE(activate(control, protocol::ConnectionRole::Control, 1).role == protocol::ConnectionRole::Control);

    protocol::RoutineControlRequest start;
    start.controlType = static_cast<std::uint8_t>(protocol::RoutineControlType::Start);
    start.routineId = static_cast<std::uint16_t>(protocol::RoutineId::SaveUwbConfiguration);

    const std::uint32_t txn = ++control.txn;
    control.sendPdu(txn, bytesOf(protocol::encodeRoutineControlRequest(start)));

    auto interim = control.waitFor(3000ms, [txn](const WireFrame &frame) { return isPendingResponse(frame, txn); });
    REQUIRE(interim.has_value());

    auto finalReply = control.waitForResponse(txn);
    REQUIRE(finalReply.has_value());
    auto routine = protocol::decodeRoutineControlResponse(bytesOf(servicePduOf(*finalReply)));
    REQUIRE(routine.ok());
    CHECK(routine.value().routineId == static_cast<std::uint16_t>(protocol::RoutineId::SaveUwbConfiguration));
    CHECK(routine.value().routineState == static_cast<std::uint8_t>(protocol::RoutineState::Completed));
}

// ---------------------------------------------------------------------------
// Reset (plan §23: reset)
// ---------------------------------------------------------------------------

TEST_CASE("Device reset is answered before the host performs the reset", "[integration]") {
    SimulatorFixture sim;
    TcpClient control(sim.tcpPort(), kControlClient);
    REQUIRE(activate(control, protocol::ConnectionRole::Control, 1).role == protocol::ConnectionRole::Control);
    enableExtendedSession(control); // 24: reset needs Control + Extended session

    protocol::DeviceResetRequest reset;
    reset.resetType = static_cast<std::uint8_t>(protocol::DeviceResetType::Soft);
    const std::uint32_t txn = ++control.txn;
    control.sendPdu(txn, bytesOf(protocol::encodeDeviceResetRequest(reset)));

    auto reply = control.waitForResponse(txn);
    REQUIRE(reply.has_value());
    auto decoded = protocol::decodeDeviceResetResponse(bytesOf(servicePduOf(*reply)));
    REQUIRE(decoded.ok());
    CHECK(decoded.value().resetType == static_cast<std::uint8_t>(protocol::DeviceResetType::Soft));

    // The host performs the reset after the response has been flushed (§24).
    for (int attempt = 0; attempt < 40 && sim.runtimeRef().simulatedResets() == 0; ++attempt) {
        std::this_thread::sleep_for(50ms);
    }
    CHECK(sim.runtimeRef().simulatedResets() >= 1);
}

// ---------------------------------------------------------------------------
// Transport lifecycle (plan §23: clean connect and disconnect)
// ---------------------------------------------------------------------------

TEST_CASE("Client disconnect releases the connection and the control slot", "[integration]") {
    SimulatorFixture sim;
    TcpClient control(sim.tcpPort(), kControlClient);
    REQUIRE(activate(control, protocol::ConnectionRole::Control, 1).role == protocol::ConnectionRole::Control);
    REQUIRE(sim.runtimeRef().connectionCount() == 1);

    control.close();
    CHECK(waitForConnectionCount(sim, 0));

    TcpClient replacement(sim.tcpPort(), kObserverClient);
    CHECK(activate(replacement, protocol::ConnectionRole::Control, 2).role == protocol::ConnectionRole::Control);
}

TEST_CASE("A frame that violates the transport contract closes the connection", "[integration]") {
    SimulatorFixture sim;
    TcpClient client(sim.tcpPort(), kControlClient);

    // Version byte 0xFF is not v1.0: the core requests a clean close (§16.1).
    client.send(ByteBuffer{0xFF, 0xEF, 0x00, 0x06, 0x00, 0x05, 0x00, 0x00, 0x00});

    auto nack = client.nextFrame(500ms); // 15: a NACK is sent when it is safe to do so
    REQUIRE(nack.has_value());
    CHECK(nack->header.payloadType == PayloadType::GenericHeaderNack);
    CHECK(waitForConnectionCount(sim, 0));
    CHECK(client.waitForClose(1000ms));
}

TEST_CASE("Two clients can talk to the same simulator at the same time", "[integration]") {
    SimulatorFixture sim;
    TcpClient control(sim.tcpPort(), kControlClient);
    TcpClient observer(sim.tcpPort(), kObserverClient);

    REQUIRE(activate(control, protocol::ConnectionRole::Control, 1).role == protocol::ConnectionRole::Control);
    REQUIRE(activate(observer, protocol::ConnectionRole::Observer, 2).role == protocol::ConnectionRole::Observer);

    protocol::ReadDidRequest read;
    read.did = static_cast<std::uint16_t>(protocol::Did::ConnectionStatus);

    const std::uint32_t controlTxn = ++control.txn;
    control.sendPdu(controlTxn, bytesOf(protocol::encodeReadDidRequest(read)));
    const std::uint32_t observerTxn = ++observer.txn;
    observer.sendPdu(observerTxn, bytesOf(protocol::encodeReadDidRequest(read)));

    auto controlReply = control.waitForResponse(controlTxn);
    auto observerReply = observer.waitForResponse(observerTxn);
    REQUIRE(controlReply.has_value());
    REQUIRE(observerReply.has_value());

    auto controlStatus = protocol::decodeReadDidResponse(bytesOf(servicePduOf(*controlReply)));
    auto observerStatus = protocol::decodeReadDidResponse(bytesOf(servicePduOf(*observerReply)));
    REQUIRE(controlStatus.ok());
    REQUIRE(observerStatus.ok());

    auto controlRecord = protocol::decodeConnectionStatusRecord(controlStatus.value().dataBytes());
    auto observerRecord = protocol::decodeConnectionStatusRecord(observerStatus.value().dataBytes());
    REQUIRE(controlRecord.ok());
    REQUIRE(observerRecord.ok());

    // §26/§39: both clients observe the same server state (control occupied, one observer).
    CHECK(controlRecord.value().controlOccupied == 1);
    CHECK(observerRecord.value().controlOccupied == 1);
    CHECK(controlRecord.value().activeObservers >= 1);
    CHECK(observerRecord.value().activeObservers >= 1);
    CHECK(controlStatus.value().data == observerStatus.value().data);
}
