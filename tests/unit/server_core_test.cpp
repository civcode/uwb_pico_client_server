#include <array>
#include <catch2/catch_all.hpp>
#include <unordered_map>

#include "uwb/protocol/capabilities.hpp"
#include "uwb/protocol/events.hpp"
#include "uwb/protocol/payloads.hpp"
#include "uwb/protocol/routines.hpp"
#include "uwb/protocol/services.hpp"
#include "uwb/protocol/uuid.hpp"
#include "uwb/server/server_core.hpp"

#include "server_fakes.hpp"
#include "test_helpers.hpp"

using namespace uwb::server;
using namespace uwb::test;
using uwb::protocol::ApplicationNackCode;
using uwb::protocol::ByteBuffer;
using uwb::protocol::ConnectionActivationRequest;
using uwb::protocol::ConnectionRole;
using uwb::protocol::ConstBytes;
using uwb::protocol::Did;
using uwb::protocol::EventId;
using uwb::protocol::GenericHeaderNackCode;
using uwb::protocol::PayloadType;
using uwb::protocol::RoutineId;
using uwb::protocol::ServiceId;
using uwb::protocol::ServiceNrc;
using uwb::protocol::TimeoutConfigRecord;
using uwb::protocol::UwbDeviceParametersRecord;
using uwb::protocol::Uuid;

namespace {

constexpr std::array<std::uint8_t, 8> kBoardId{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
constexpr std::uint16_t kServerAddress = 0x1000;
constexpr std::uint16_t kClientA = 0x0E01;
constexpr std::uint16_t kClientB = 0x0E02;

constexpr std::uint8_t sidOf(ServiceId service) noexcept { return static_cast<std::uint8_t>(service); }
constexpr std::uint8_t positiveSid(ServiceId service) noexcept {
    return static_cast<std::uint8_t>(static_cast<std::uint8_t>(service) + uwb::protocol::kPositiveResponseSidOffset);
}

class Harness {
public:
    // 'config' aliases the configuration the core actually uses, so a test can
    // change policy switches after construction.
    Harness() : config(deps.config) {
        core.configureIdentity(kBoardId, "pico-anchor", kServerAddress, 45678);
        core.setCapabilityMasks(uwb::protocol::kKnownCapabilityMask, uwb::protocol::kKnownCapabilityMask);
        // configureIdentity() raises the initial re-init request; tests assert on it explicitly.
        core.clearBackendReinitRequest();

    }

    FakeConnectionWriter &connect(ConnectionId id) {
        FakeConnectionWriter &writer = writers[id];
        core.onConnect(id, writer, PeerInfo{static_cast<std::uint16_t>(id + 40000)});
        return writer;
    }

    ByteBuffer activationFrame(ConnectionRole role, std::uint16_t clientAddress = kClientA) {
        ConnectionActivationRequest request;
        request.clientLogicalAddress = clientAddress;
        request.requestedRole = role;
        request.flags = 0;
        return frame::activation(request);
    }

    // `servicePdu` is a complete service PDU (SID + body); the protocol encoders
    // already include the service id.
    ByteBuffer request(ServiceId service, ConstBytes servicePdu, std::uint32_t transactionId = 0x1234,
                       std::uint16_t clientAddress = kClientA, bool ackRequired = true) {
        (void)service;
        return frame::client(clientAddress, transactionId, servicePdu, kServerAddress, ackRequired);
    }

    ByteBuffer clientPresent(std::uint32_t transactionId = 0x1234, std::uint16_t clientAddress = kClientA) {
        return request(ServiceId::ClientPresent,
                       uwb::protocol::bytesOf(uwb::protocol::encodeClientPresentRequest()), transactionId,
                       clientAddress);
    }

    std::vector<frame::ServerFrame> send(ConnectionId id, const ByteBuffer &bytes) {
        core.onBytes(id, uwb::protocol::ConstBytes{bytes.data(), bytes.size()});
        core.tick();
        return drain(id);
    }

    std::vector<frame::ServerFrame> pump(ConnectionId id) {
        core.tick();
        return drain(id);
    }

    std::vector<frame::ServerFrame> drain(ConnectionId id) {
        auto it = writers.find(id);
        if (it == writers.end()) {
            return {};
        }
        std::vector<frame::ServerFrame> out = frame::parse(it->second.frames());
        it->second.clear();
        return out;
    }

    bool activate(ConnectionId id, ConnectionRole role, std::uint16_t clientAddress = kClientA) {
        const auto frames = send(id, activationFrame(role, clientAddress));
        for (const frame::ServerFrame &f : frames) {
            if (f.type == PayloadType::ConnectionActivationResponse) {
                auto decoded = uwb::protocol::decodeConnectionActivationResponse(
                    uwb::protocol::ConstBytes{f.payload.data(), f.payload.size()});
                return decoded.ok() && decoded.value().assignedRole == role;
            }
        }
        return false;
    }

    void enterExtendedSession(ConnectionId id) {
        uwb::protocol::SessionControlRequest sessionRequest;
        sessionRequest.requestedSession = static_cast<std::uint8_t>(uwb::protocol::SessionId::Extended);
        (void)send(id, request(ServiceId::SessionControl,
                               uwb::protocol::bytesOf(uwb::protocol::encodeSessionControlRequest(sessionRequest))));
    }

    FakeClock clock{1'000'000};
    FakeStorage storage;
    FakeSecurityProvider security;
    FakeBackend backend;
    FakeLogger logger;
    ServerCore::Dependencies deps{ServerConfig{}, clock, storage, &security, &backend, &logger};
    ServerConfig &config;
    ServerCore core{deps};
    std::unordered_map<ConnectionId, FakeConnectionWriter> writers;
};

const frame::ServerFrame *findFirst(const std::vector<frame::ServerFrame> &frames, PayloadType type) {
    for (const frame::ServerFrame &f : frames) {
        if (f.type == type) {
            return &f;
        }
    }
    return nullptr;
}

ConstBytes payloadOf(const frame::ServerFrame &frame) {
    return ConstBytes{frame.payload.data(), frame.payload.size()};
}

} // namespace

// ---------------------------------------------------------------------------
// Activation and role policy (specification §9, §18)
// ---------------------------------------------------------------------------

TEST_CASE("activation grants the requested role and reports server identity", "[unit][server][activation]") {
    Harness h;
    h.connect(1);

    const auto frames = h.send(1, h.activationFrame(ConnectionRole::Control));
    const frame::ServerFrame *response = findFirst(frames, PayloadType::ConnectionActivationResponse);
    REQUIRE(response != nullptr);

    auto decoded = uwb::protocol::decodeConnectionActivationResponse(payloadOf(*response));
    REQUIRE(decoded.ok());
    CHECK(decoded.value().serverLogicalAddress == kServerAddress);
    CHECK(decoded.value().assignedRole == ConnectionRole::Control);
    CHECK(decoded.value().responseCode == uwb::protocol::ActivationResponseCode::AcceptedRequestedRole);
    CHECK(decoded.value().serverMaxPayload == uwb::protocol::kMaxProtocolPayload);
    CHECK(uwb::protocol::hasCapability(decoded.value().capabilityMask, uwb::protocol::Capability::Range));
    CHECK_FALSE(decoded.value().deviceUuid.isZero());
    CHECK(h.core.wantsConnectionOpen(1));
}

TEST_CASE("the second Control request is downgraded to Observer", "[unit][server][activation]") {
    Harness h;
    h.connect(1);
    h.connect(2);

    CHECK(h.activate(1, ConnectionRole::Control, kClientA));

    const auto frames = h.send(2, h.activationFrame(ConnectionRole::Control, kClientB));
    const frame::ServerFrame *response = findFirst(frames, PayloadType::ConnectionActivationResponse);
    REQUIRE(response != nullptr);

    auto decoded = uwb::protocol::decodeConnectionActivationResponse(payloadOf(*response));
    REQUIRE(decoded.ok());
    CHECK(decoded.value().assignedRole == ConnectionRole::Observer);
    CHECK(decoded.value().responseCode == uwb::protocol::ActivationResponseCode::AcceptedDowngradedToObserver);
    CHECK(h.core.connections().controlCount() == 1);
}

TEST_CASE("an invalid client address is refused and the connection is closed",
          "[unit][server][activation]") {
    Harness h;
    h.connect(1);

    const auto frames = h.send(1, h.activationFrame(ConnectionRole::Control, 0x0010));
    const frame::ServerFrame *response = findFirst(frames, PayloadType::ConnectionActivationResponse);
    REQUIRE(response != nullptr);
    auto decoded = uwb::protocol::decodeConnectionActivationResponse(payloadOf(*response));
    REQUIRE(decoded.ok());
    CHECK(decoded.value().assignedRole == ConnectionRole::None);
    CHECK(decoded.value().responseCode == uwb::protocol::ActivationResponseCode::RejectedRolePolicyDenied);

    // §18: the response is flushed first, then the connection is closed.
    CHECK_FALSE(h.core.wantsConnectionOpen(1));
}

TEST_CASE("application messages before activation are NACKed", "[unit][server][activation]") {
    Harness h;
    h.connect(1);

    const auto frames = h.send(1, h.request(ServiceId::ReadDataByIdentifier,
                                           uwb::protocol::bytesOf(uwb::protocol::encodeReadDidRequest(
                                               uwb::protocol::ReadDidRequest{static_cast<std::uint16_t>(Did::DeviceUuid)}))));

    const frame::ServerFrame *nack = findFirst(frames, PayloadType::ApplicationMessageNack);
    REQUIRE(nack != nullptr);
    auto decoded = uwb::protocol::decodeApplicationMessageNack(payloadOf(*nack));
    REQUIRE(decoded.ok());
    CHECK(decoded.value().code == ApplicationNackCode::ConnectionNotActivated);
    CHECK_FALSE(h.core.wantsConnectionOpen(1)); // respond, then close
}

TEST_CASE("a client must not send server-to-client payload types", "[unit][server][framing]") {
    Harness h;
    h.connect(1);

    const ByteBuffer junk = uwb::protocol::encodeEventNotification(uwb::protocol::EventNotification{});
    const auto frames = h.send(1, frame::raw(PayloadType::EventNotification, uwb::protocol::bytesOf(junk)));

    const frame::ServerFrame *nack = findFirst(frames, PayloadType::GenericHeaderNack);
    REQUIRE(nack != nullptr);
    auto decoded = uwb::protocol::decodeGenericHeaderNack(payloadOf(*nack));
    REQUIRE(decoded.ok());
    CHECK(decoded.value().code == GenericHeaderNackCode::InvalidInCurrentState);
    CHECK_FALSE(h.core.wantsConnectionOpen(1));
}

TEST_CASE("a corrupt generic header is answered and closes the connection", "[unit][server][framing]") {
    Harness h;
    h.connect(1);

    ByteBuffer broken{0x10, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x01}; // inverse version wrong
    const auto frames = h.send(1, broken);

    const frame::ServerFrame *nack = findFirst(frames, PayloadType::GenericHeaderNack);
    REQUIRE(nack != nullptr);
    auto decoded = uwb::protocol::decodeGenericHeaderNack(payloadOf(*nack));
    REQUIRE(decoded.ok());
    CHECK(decoded.value().code == GenericHeaderNackCode::IncorrectHeaderPattern);
    CHECK_FALSE(h.core.wantsConnectionOpen(1));
}

// ---------------------------------------------------------------------------
// Envelope handling (specification §20)
// ---------------------------------------------------------------------------

TEST_CASE("an accepted request is ACKed and answered", "[unit][server][envelope]") {
    Harness h;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Control));

    const auto frames = h.send(1, h.clientPresent());

    CHECK(findFirst(frames, PayloadType::ApplicationMessageAck) != nullptr);
    const frame::ServerFrame *answer = findFirst(frames, PayloadType::ApplicationMessage);
    REQUIRE(answer != nullptr);
    CHECK(frame::sid(*answer) == positiveSid(ServiceId::ClientPresent));
    CHECK(frame::envelope(*answer).transactionId == 0x1234);
    CHECK(frame::envelope(*answer).targetLogicalAddress == kClientA);
    CHECK(frame::envelope(*answer).sourceLogicalAddress == kServerAddress);
}

TEST_CASE("an unknown service id is answered with serviceNotSupported", "[unit][server][envelope]") {
    Harness h;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Observer));

    const auto frames = h.send(1, h.request(static_cast<ServiceId>(0x55), ByteBuffer{0x55})); // no such service

    const auto nrc = findFirst(frames, PayloadType::ApplicationMessage);
    REQUIRE(nrc != nullptr);
    CHECK(frame::nrc(*nrc) == ServiceNrc::ServiceNotSupported);
}

TEST_CASE("a wrong target address is NACKed", "[unit][server][envelope]") {
    Harness h;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Observer));

    ByteBuffer pdu;
    pdu.push_back(sidOf(ServiceId::ClientPresent));
    const ByteBuffer message = frame::client(kClientA, 7, uwb::protocol::bytesOf(pdu), 0x1234, true);
    const auto frames = h.send(1, message);

    const frame::ServerFrame *nack = findFirst(frames, PayloadType::ApplicationMessageNack);
    REQUIRE(nack != nullptr);
    auto decoded = uwb::protocol::decodeApplicationMessageNack(payloadOf(*nack));
    REQUIRE(decoded.ok());
    CHECK(decoded.value().code == ApplicationNackCode::InvalidSourceOrTargetAddress);
}

// ---------------------------------------------------------------------------
// Session Control and Security Access (specification §22, §23, §25)
// ---------------------------------------------------------------------------

TEST_CASE("Control can enter Extended Session and learns the server timeouts",
          "[unit][server][session]") {
    Harness h;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Control));

    uwb::protocol::SessionControlRequest sessionRequest;
    sessionRequest.requestedSession = static_cast<std::uint8_t>(uwb::protocol::SessionId::Extended);
    const auto frames = h.send(1, h.request(ServiceId::SessionControl, uwb::protocol::bytesOf(
                                      uwb::protocol::encodeSessionControlRequest(sessionRequest))));

    const frame::ServerFrame *answer = findFirst(frames, PayloadType::ApplicationMessage);
    REQUIRE(answer != nullptr);
    auto pdu = frame::servicePdu(*answer);
    auto decoded = uwb::protocol::decodeSessionControlResponse(pdu);
    REQUIRE(decoded.ok());
    CHECK(decoded.value().activeSession == static_cast<std::uint8_t>(uwb::protocol::SessionId::Extended));
    CHECK(decoded.value().p2ServerMaxMs == h.config.p2ServerMaxMs);
    CHECK(decoded.value().p2StarServerMax10ms == h.config.p2StarServerMax10ms);
    CHECK(h.core.connections().find(1)->isExtendedSession());
}

TEST_CASE("an Observer cannot enter Extended Session", "[unit][server][session]") {
    Harness h;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Observer));

    uwb::protocol::SessionControlRequest sessionRequest;
    sessionRequest.requestedSession = static_cast<std::uint8_t>(uwb::protocol::SessionId::Extended);
    const auto frames = h.send(1, h.request(ServiceId::SessionControl, uwb::protocol::bytesOf(
                                      uwb::protocol::encodeSessionControlRequest(sessionRequest))));

    const frame::ServerFrame *answer = findFirst(frames, PayloadType::ApplicationMessage);
    REQUIRE(answer != nullptr);
    CHECK(frame::nrc(*answer) == ServiceNrc::ConditionsNotCorrect);
    CHECK_FALSE(h.core.connections().find(1)->isExtendedSession());
}

TEST_CASE("Security Access is refused when the device is not protected", "[unit][server][security]") {
    Harness h;
    h.config.securityEnabled = false;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Control));

    uwb::protocol::SecurityAccessRequest access;
    access.subFunction = static_cast<std::uint8_t>(uwb::protocol::SecuritySubFunction::RequestSeed);
    access.key = 0;
    const auto frames = h.send(1, h.request(ServiceId::SecurityAccess,
                                           uwb::protocol::bytesOf(uwb::protocol::encodeSecurityAccessRequest(access))));

    const frame::ServerFrame *answer = findFirst(frames, PayloadType::ApplicationMessage);
    REQUIRE(answer != nullptr);
    CHECK(frame::nrc(*answer) == ServiceNrc::ServiceNotSupportedInActiveSession);
    CHECK(h.security.seedCalls_ == 0);
}

TEST_CASE("seed then key unlocks the connection for Extended Session", "[unit][server][security]") {
    Harness h;
    h.config.securityEnabled = true;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Control));

    uwb::protocol::SecurityAccessRequest seedRequest;
    seedRequest.subFunction = static_cast<std::uint8_t>(uwb::protocol::SecuritySubFunction::RequestSeed);
    seedRequest.key = 0;
    const auto seedFrames = h.send(1, h.request(ServiceId::SecurityAccess,
                                                uwb::protocol::bytesOf(uwb::protocol::encodeSecurityAccessRequest(
                                                    seedRequest))));

    const frame::ServerFrame *seedAnswer = findFirst(seedFrames, PayloadType::ApplicationMessage);
    REQUIRE(seedAnswer != nullptr);
    auto seedPdu = frame::servicePdu(*seedAnswer);
    auto seed = uwb::protocol::decodeSecurityAccessResponse(seedPdu);
    REQUIRE(seed.ok());
    CHECK(seed.value().seed == h.security.seedValue);
    CHECK(h.core.connections().find(1)->security == SecurityState::SeedIssued);

    // Extended Session is only allowed after the key is accepted (§23 + §25).
    uwb::protocol::SessionControlRequest sessionRequest;
    sessionRequest.requestedSession = static_cast<std::uint8_t>(uwb::protocol::SessionId::Extended);
    const auto beforeKey = h.send(1, h.request(ServiceId::SessionControl,
                                               uwb::protocol::bytesOf(uwb::protocol::encodeSessionControlRequest(
                                                   sessionRequest))));
    CHECK(frame::nrc(*findFirst(beforeKey, PayloadType::ApplicationMessage)) == ServiceNrc::SecurityAccessDenied);

    uwb::protocol::SecurityAccessRequest keyRequest;
    keyRequest.subFunction = static_cast<std::uint8_t>(uwb::protocol::SecuritySubFunction::SendKey);
    keyRequest.key = h.security.expectedKey;
    const auto keyFrames = h.send(1, h.request(ServiceId::SecurityAccess,
                                               uwb::protocol::bytesOf(uwb::protocol::encodeSecurityAccessRequest(
                                                   keyRequest))));
    const frame::ServerFrame *keyAnswer = findFirst(keyFrames, PayloadType::ApplicationMessage);
    REQUIRE(keyAnswer != nullptr);
    CHECK(frame::sid(*keyAnswer) == positiveSid(ServiceId::SecurityAccess));
    CHECK(h.core.connections().find(1)->isUnlocked());

    const auto afterKey = h.send(1, h.request(ServiceId::SessionControl,
                                              uwb::protocol::bytesOf(uwb::protocol::encodeSessionControlRequest(
                                                  sessionRequest))));
    auto session = uwb::protocol::decodeSessionControlResponse(
        frame::servicePdu(*findFirst(afterKey, PayloadType::ApplicationMessage)));
    REQUIRE(session.ok());
    CHECK(session.value().activeSession == static_cast<std::uint8_t>(uwb::protocol::SessionId::Extended));
}

TEST_CASE("a wrong key is rejected with invalidKey and relocks the seed", "[unit][server][security]") {
    Harness h;
    h.config.securityEnabled = true;
    h.security.decision = uwb::server::SecurityDecision::BadKey;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Control));

    uwb::protocol::SecurityAccessRequest seedRequest;
    seedRequest.subFunction = static_cast<std::uint8_t>(uwb::protocol::SecuritySubFunction::RequestSeed);
    (void)h.send(1, h.request(ServiceId::SecurityAccess,
                              uwb::protocol::bytesOf(uwb::protocol::encodeSecurityAccessRequest(seedRequest))));

    uwb::protocol::SecurityAccessRequest keyRequest;
    keyRequest.subFunction = static_cast<std::uint8_t>(uwb::protocol::SecuritySubFunction::SendKey);
    keyRequest.key = 0x0BADF00DU;
    const auto frames = h.send(1, h.request(ServiceId::SecurityAccess,
                                            uwb::protocol::bytesOf(uwb::protocol::encodeSecurityAccessRequest(
                                                keyRequest))));

    CHECK(frame::nrc(*findFirst(frames, PayloadType::ApplicationMessage)) == ServiceNrc::InvalidKey);
    CHECK(h.security.failureCalls_ == 1);
    CHECK(h.core.connections().find(1)->security == SecurityState::Locked); // §25: a new seed is required
}

TEST_CASE("sending a key without a seed is a sequence error", "[unit][server][security]") {
    Harness h;
    h.config.securityEnabled = true;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Control));

    uwb::protocol::SecurityAccessRequest keyRequest;
    keyRequest.subFunction = static_cast<std::uint8_t>(uwb::protocol::SecuritySubFunction::SendKey);
    keyRequest.key = h.security.expectedKey;
    const auto frames = h.send(1, h.request(ServiceId::SecurityAccess,
                                            uwb::protocol::bytesOf(uwb::protocol::encodeSecurityAccessRequest(
                                                keyRequest))));

    CHECK(frame::nrc(*findFirst(frames, PayloadType::ApplicationMessage)) == ServiceNrc::RequestSequenceError);
}

// ---------------------------------------------------------------------------
// DID services (specification §26, §27)
// ---------------------------------------------------------------------------

TEST_CASE("a core-owned DID is answered synchronously", "[unit][server][did]") {
    Harness h;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Observer));

    const auto frames = h.send(1, h.request(ServiceId::ReadDataByIdentifier,
                                            uwb::protocol::bytesOf(uwb::protocol::encodeReadDidRequest(
                                                uwb::protocol::ReadDidRequest{static_cast<std::uint16_t>(Did::DeviceName)}))));

    const frame::ServerFrame *answer = findFirst(frames, PayloadType::ApplicationMessage);
    REQUIRE(answer != nullptr);
    auto response = uwb::protocol::decodeReadDidResponse(frame::servicePdu(*answer));
    REQUIRE(response.ok());
    CHECK(response.value().did == static_cast<std::uint16_t>(Did::DeviceName));
    auto name = uwb::protocol::decodeTextRecord8(uwb::protocol::bytesOf(response.value().data),
                                                 uwb::protocol::kMaxDeviceNameLength);
    REQUIRE(name.ok());
    CHECK(std::string(name.value().begin(), name.value().end()) == "pico-anchor");
}

TEST_CASE("an unknown DID is answered with requestOutOfRange", "[unit][server][did]") {
    Harness h;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Observer));

    const auto frames = h.send(1, h.request(ServiceId::ReadDataByIdentifier,
                                            uwb::protocol::bytesOf(uwb::protocol::encodeReadDidRequest(
                                                uwb::protocol::ReadDidRequest{0xFABC}))));
    CHECK(frame::nrc(*findFirst(frames, PayloadType::ApplicationMessage)) == ServiceNrc::RequestOutOfRange);
}

TEST_CASE("a UWB-backed DID answers 0x78 then the real record", "[unit][server][did][async]") {
    Harness h;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Observer));

    const auto pending = h.send(1, h.request(ServiceId::ReadDataByIdentifier,
                                             uwb::protocol::bytesOf(uwb::protocol::encodeReadDidRequest(
                                                 uwb::protocol::ReadDidRequest{
                                                     static_cast<std::uint16_t>(Did::UwbDeviceParameters)}))));

    const frame::ServerFrame *pendingAnswer = findFirst(pending, PayloadType::ApplicationMessage);
    REQUIRE(pendingAnswer != nullptr);
    CHECK(frame::nrc(*pendingAnswer) == ServiceNrc::ResponsePending);
    CHECK(h.core.pendingRequestCount() == 1);
    CHECK(h.backend.countOf(FakeBackend::Submission::Kind::DidRead) == 1);

    const ByteBuffer record = uwb::protocol::encodeRecord(uwb::protocol::UwbDeviceParametersRecord{});
    const std::size_t recordSize = record.size();
    ByteBuffer delivered = record;
    h.backend.completeRead(0, DidReadResult{BackendStatus::Completed, std::move(delivered)});

    const auto finalFrames = h.pump(1);
    const frame::ServerFrame *answer = findFirst(finalFrames, PayloadType::ApplicationMessage);
    REQUIRE(answer != nullptr);
    auto response = uwb::protocol::decodeReadDidResponse(frame::servicePdu(*answer));
    REQUIRE(response.ok());
    CHECK(response.value().did == static_cast<std::uint16_t>(Did::UwbDeviceParameters));
    CHECK(response.value().data.size() == recordSize);
    CHECK(sameBytes(uwb::protocol::ConstBytes{response.value().data.data(), response.value().data.size()},
                    uwb::protocol::ConstBytes{record.data(), record.size()}));
    CHECK(h.core.pendingRequestCount() == 0);
}

TEST_CASE("an unfinished backend read gets a final NRC after p2*", "[unit][server][did][async][timeout]") {
    Harness h;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Observer));

    (void)h.send(1, h.request(ServiceId::ReadDataByIdentifier,
                              uwb::protocol::bytesOf(uwb::protocol::encodeReadDidRequest(
                                  uwb::protocol::ReadDidRequest{
                                      static_cast<std::uint16_t>(Did::UwbDeviceParameters)}))));

    const std::uint64_t p2StarMs = static_cast<std::uint64_t>(h.config.p2StarServerMax10ms) * 10ULL;
    h.clock.advanceMs(p2StarMs + 1);
    const auto frames = h.pump(1);

    CHECK(frame::nrc(*findFirst(frames, PayloadType::ApplicationMessage)) == ServiceNrc::GeneralProgrammingFailure);
    CHECK(h.core.pendingRequestCount() == 0);
    const FakeBackend::Submission *submission = h.backend.findByOperation(1);
    REQUIRE(submission != nullptr);
    CHECK(submission->cancelled); // the core must release the UWB channel (§9.4)
}

TEST_CASE("a backend failure is reported as a negative response", "[unit][server][did][async]") {
    Harness h;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Observer));

    (void)h.send(1, h.request(ServiceId::ReadDataByIdentifier,
                              uwb::protocol::bytesOf(uwb::protocol::encodeReadDidRequest(
                                  uwb::protocol::ReadDidRequest{
                                      static_cast<std::uint16_t>(Did::UwbDeviceParameters)}))));
    h.backend.completeRead(0, DidReadResult{BackendStatus::Timeout, {}});

    const auto frames = h.pump(1);
    CHECK(frame::nrc(*findFirst(frames, PayloadType::ApplicationMessage)) == ServiceNrc::GeneralProgrammingFailure);
}

TEST_CASE("the backend queue being full answers busyRepeatRequest", "[unit][server][did][async]") {
    Harness h;
    h.backend.accept_ = false; // submission refused because the UWB queue is full (§53)
    h.backend.busy_ = true;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Observer));

    const auto frames = h.send(1, h.request(ServiceId::ReadDataByIdentifier,
                                            uwb::protocol::bytesOf(uwb::protocol::encodeReadDidRequest(
                                                uwb::protocol::ReadDidRequest{
                                                    static_cast<std::uint16_t>(Did::UwbDeviceParameters)}))));

    CHECK(frame::nrc(*findFirst(frames, PayloadType::ApplicationMessage)) == ServiceNrc::BusyRepeatRequest);
    CHECK(h.core.pendingRequestCount() == 0);
}

TEST_CASE("write DID policy is enforced end to end", "[unit][server][did][policy]") {
    Harness h;
    h.connect(1);
    h.connect(2);
    REQUIRE(h.activate(1, ConnectionRole::Control, kClientA));
    REQUIRE(h.activate(2, ConnectionRole::Observer, kClientB));

    const ByteBuffer record = uwb::protocol::encodeRecord(TimeoutConfigRecord{1000, 100, 5000, 60000, 10000});
    const ByteBuffer writeBody = uwb::protocol::encodeWriteDidRequest(uwb::protocol::WriteDidRequest{
        static_cast<std::uint16_t>(Did::TimeoutConfiguration), record});

    // Observer: refused (§9.2).
    const auto observerWrite = h.send(2, h.request(ServiceId::WriteDataByIdentifier, uwb::protocol::bytesOf(writeBody),
                                                   0x21, kClientB));
    CHECK(frame::nrc(*findFirst(observerWrite, PayloadType::ApplicationMessage)) == ServiceNrc::ConditionsNotCorrect);

    // Control in Default Session: refused (§22.3).
    const auto defaultSession = h.send(1, h.request(ServiceId::WriteDataByIdentifier,
                                                    uwb::protocol::bytesOf(writeBody)));
    CHECK(frame::nrc(*findFirst(defaultSession, PayloadType::ApplicationMessage)) == ServiceNrc::ConditionsNotCorrect);

    // Control in Extended Session: accepted and persisted.
    h.enterExtendedSession(1);
    const auto accepted = h.send(1, h.request(ServiceId::WriteDataByIdentifier, uwb::protocol::bytesOf(writeBody)));
    const frame::ServerFrame *answer = findFirst(accepted, PayloadType::ApplicationMessage);
    REQUIRE(answer != nullptr);
    CHECK(frame::sid(*answer) == positiveSid(ServiceId::WriteDataByIdentifier));
    CHECK(h.storage.commitCalls_ == 1);

    auto readBack = h.send(1, h.request(ServiceId::ReadDataByIdentifier,
                                        uwb::protocol::bytesOf(uwb::protocol::encodeReadDidRequest(
                                            uwb::protocol::ReadDidRequest{
                                                static_cast<std::uint16_t>(Did::TimeoutConfiguration)}))));
    auto response = uwb::protocol::decodeReadDidResponse(frame::servicePdu(*findFirst(readBack, PayloadType::ApplicationMessage)));
    REQUIRE(response.ok());
    auto timeouts = uwb::protocol::decodeTimeoutConfigRecord(uwb::protocol::bytesOf(response.value().data));
    REQUIRE(timeouts.ok());
    CHECK(timeouts.value().uartCommandTimeoutMs == 1000);
}

TEST_CASE("an Observer cannot write a UWB-backed DID either", "[unit][server][did][policy]") {
    Harness h;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Observer));

    ByteBuffer record = uwb::protocol::encodeRecord(uwb::protocol::UwbDeviceParametersRecord{});
    const auto frames = h.send(1, h.request(ServiceId::WriteDataByIdentifier,
                                            uwb::protocol::bytesOf(uwb::protocol::encodeWriteDidRequest(
                                                uwb::protocol::WriteDidRequest{
                                                    static_cast<std::uint16_t>(Did::UwbDeviceParameters), record}))));
    CHECK(frame::nrc(*findFirst(frames, PayloadType::ApplicationMessage)) == ServiceNrc::ConditionsNotCorrect);
    CHECK(h.backend.countOf(FakeBackend::Submission::Kind::DidWrite) == 0);
}

TEST_CASE("a UWB-backed write is asynchronous and answers 0x78 first", "[unit][server][did][async]") {
    Harness h;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Control));
    h.enterExtendedSession(1);

    ByteBuffer record = uwb::protocol::encodeRecord(uwb::protocol::UwbDeviceParametersRecord{});
    const auto pending = h.send(1, h.request(ServiceId::WriteDataByIdentifier,
                                             uwb::protocol::bytesOf(uwb::protocol::encodeWriteDidRequest(
                                                 uwb::protocol::WriteDidRequest{
                                                     static_cast<std::uint16_t>(Did::UwbDeviceParameters), record}))));
    CHECK(frame::nrc(*findFirst(pending, PayloadType::ApplicationMessage)) == ServiceNrc::ResponsePending);

    h.backend.completeWrite(0, DidWriteResult{BackendStatus::Completed});
    const auto frames = h.pump(1);
    const frame::ServerFrame *answer = findFirst(frames, PayloadType::ApplicationMessage);
    REQUIRE(answer != nullptr);
    CHECK(frame::sid(*answer) == positiveSid(ServiceId::WriteDataByIdentifier));
    CHECK(h.backend.submissions_[0].payload == record); // the exact record bytes reach the backend
}

// ---------------------------------------------------------------------------
// Routine Control (specification §28, §42)
// ---------------------------------------------------------------------------

TEST_CASE("a server-local routine completes immediately and requests backend re-init",
          "[unit][server][routine]") {
    Harness h;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Control));
    h.enterExtendedSession(1);

    const ByteBuffer body = uwb::protocol::encodeRoutineControlRequest(uwb::protocol::RoutineControlRequest{
        static_cast<std::uint8_t>(uwb::protocol::RoutineControlType::Start),
        static_cast<std::uint16_t>(RoutineId::InitializeUwbBackend), {}});
    const auto frames = h.send(1, h.request(ServiceId::RoutineControl, uwb::protocol::bytesOf(body)));

    const frame::ServerFrame *answer = findFirst(frames, PayloadType::ApplicationMessage);
    REQUIRE(answer != nullptr);
    auto response = uwb::protocol::decodeRoutineControlResponse(frame::servicePdu(*answer));
    REQUIRE(response.ok());
    CHECK(response.value().routineState == static_cast<std::uint8_t>(uwb::protocol::RoutineState::Completed));
    CHECK(h.core.backendReinitRequested());
    CHECK(h.backend.pendingOperations() == 0);
}

TEST_CASE("a backend routine answers 0x78 then the final routine state", "[unit][server][routine][async]") {
    Harness h;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Control));
    h.enterExtendedSession(1);

    const ByteBuffer start = uwb::protocol::encodeRoutineControlRequest(uwb::protocol::RoutineControlRequest{
        static_cast<std::uint8_t>(uwb::protocol::RoutineControlType::Start),
        static_cast<std::uint16_t>(RoutineId::UwbMeasurementAcquisition), {}});
    const auto pending = h.send(1, h.request(ServiceId::RoutineControl, uwb::protocol::bytesOf(start)));
    CHECK(frame::nrc(*findFirst(pending, PayloadType::ApplicationMessage)) == ServiceNrc::ResponsePending);
    CHECK(h.backend.countOf(FakeBackend::Submission::Kind::Routine) == 1);

    h.backend.completeRoutine(0, RoutineResult{BackendStatus::Completed, ByteBuffer{0x01, 0x02}});
    const auto frames = h.pump(1);
    auto response = uwb::protocol::decodeRoutineControlResponse(
        frame::servicePdu(*findFirst(frames, PayloadType::ApplicationMessage)));
    REQUIRE(response.ok());
    CHECK(response.value().routineState == static_cast<std::uint8_t>(uwb::protocol::RoutineState::Completed));
    CHECK(response.value().statusRecord.size() == 2);
}

TEST_CASE("an Observer cannot start a routine", "[unit][server][routine][policy]") {
    Harness h;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Observer));

    const ByteBuffer start = uwb::protocol::encodeRoutineControlRequest(uwb::protocol::RoutineControlRequest{
        static_cast<std::uint8_t>(uwb::protocol::RoutineControlType::Start),
        static_cast<std::uint16_t>(RoutineId::AddTag), {}});
    const auto frames = h.send(1, h.request(ServiceId::RoutineControl, uwb::protocol::bytesOf(start)));
    CHECK(frame::nrc(*findFirst(frames, PayloadType::ApplicationMessage)) == ServiceNrc::ConditionsNotCorrect);
}

TEST_CASE("stopping a non-cancellable routine is refused", "[unit][server][routine]") {
    Harness h;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Control));
    h.enterExtendedSession(1);

    const ByteBuffer start = uwb::protocol::encodeRoutineControlRequest(uwb::protocol::RoutineControlRequest{
        static_cast<std::uint8_t>(uwb::protocol::RoutineControlType::Start),
        static_cast<std::uint16_t>(RoutineId::AddTag), {}});
    (void)h.send(1, h.request(ServiceId::RoutineControl, uwb::protocol::bytesOf(start)));

    const ByteBuffer stop = uwb::protocol::encodeRoutineControlRequest(uwb::protocol::RoutineControlRequest{
        static_cast<std::uint8_t>(uwb::protocol::RoutineControlType::Stop),
        static_cast<std::uint16_t>(RoutineId::AddTag), {}});
    const auto frames = h.send(1, h.request(ServiceId::RoutineControl, uwb::protocol::bytesOf(stop), 0x99));
    CHECK(frame::nrc(*findFirst(frames, PayloadType::ApplicationMessage)) == ServiceNrc::ConditionsNotCorrect);

    // Measurement acquisition is cancellable (§42): start it, then Stop moves it
    // to Stopping instead of finishing immediately.
    const ByteBuffer startAcquisition = uwb::protocol::encodeRoutineControlRequest(
        uwb::protocol::RoutineControlRequest{static_cast<std::uint8_t>(uwb::protocol::RoutineControlType::Start),
                                             static_cast<std::uint16_t>(RoutineId::UwbMeasurementAcquisition), {}});
    const auto startFrames =
        h.send(1, h.request(ServiceId::RoutineControl, uwb::protocol::bytesOf(startAcquisition), 0x97));
    CHECK(frame::nrc(*findFirst(startFrames, PayloadType::ApplicationMessage)) == ServiceNrc::ResponsePending);

    const ByteBuffer cancel = uwb::protocol::encodeRoutineControlRequest(uwb::protocol::RoutineControlRequest{
        static_cast<std::uint8_t>(uwb::protocol::RoutineControlType::Stop),
        static_cast<std::uint16_t>(RoutineId::UwbMeasurementAcquisition), {}});
    (void)h.send(1, h.request(ServiceId::RoutineControl, uwb::protocol::bytesOf(cancel), 0x98));
    const auto results = h.send(1, h.request(ServiceId::RoutineControl,
                                             uwb::protocol::bytesOf(uwb::protocol::encodeRoutineControlRequest(
                                                 uwb::protocol::RoutineControlRequest{
                                                     static_cast<std::uint8_t>(
                                                         uwb::protocol::RoutineControlType::RequestResults),
                                                     static_cast<std::uint16_t>(
                                                         RoutineId::UwbMeasurementAcquisition),
                                                     {}})),
                                             0x97));
    auto response = uwb::protocol::decodeRoutineControlResponse(
        frame::servicePdu(*findFirst(results, PayloadType::ApplicationMessage)));
    REQUIRE(response.ok());
    CHECK(response.value().routineState == static_cast<std::uint8_t>(uwb::protocol::RoutineState::Stopping));
}

TEST_CASE("an Observer may read DIDs and subscribe to events", "[unit][server][policy]") {
    Harness h;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Observer));

    // §9.2: Observers keep read access.
    const ByteBuffer body = uwb::protocol::encodeReadDidRequest(
        uwb::protocol::ReadDidRequest{static_cast<std::uint16_t>(Did::ConnectionStatus)});
    const auto frames = h.send(1, h.request(ServiceId::ReadDataByIdentifier, uwb::protocol::bytesOf(body)));
    const auto *answer = findFirst(frames, PayloadType::ApplicationMessage);
    REQUIRE(answer != nullptr);
    CHECK(frame::sid(*answer) == positiveSid(ServiceId::ReadDataByIdentifier));

    // §9.2: Observers may subscribe to measurement events.
    const ByteBuffer subscribe = uwb::protocol::encodeEventSubscribeRequest(uwb::protocol::EventSubscribeRequest{
        static_cast<std::uint16_t>(EventId::UwbMeasurement),
        static_cast<std::uint8_t>(uwb::protocol::StreamMode::Live), 0, 0});
    const auto subFrames = h.send(1, h.request(ServiceId::EventControl, uwb::protocol::bytesOf(subscribe), 0xABCD));
    const auto sub =
        uwb::protocol::decodeEventSubscribeResponse(frame::servicePdu(*findFirst(subFrames, PayloadType::ApplicationMessage)));
    REQUIRE(sub.ok());
}

TEST_CASE("ClientPresent extends the session inactivity deadline", "[unit][server][session]") {
    Harness h;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Control));
    h.enterExtendedSession(1);

    // Advance close to the session inactivity timeout, then refresh with ClientPresent.
    for (int i = 0; i < 45; ++i) {
        (void)h.pump(1);
        h.clock.advanceMs(100); // 4.5 s
    }
    const auto refreshed = h.send(1, h.clientPresent(0xB007));
    CHECK(frame::sid(*findFirst(refreshed, PayloadType::ApplicationMessage)) == positiveSid(ServiceId::ClientPresent));

    // Another 4.5 s must not expire the session yet (§29 refresh + §52 timeout).
    for (int i = 0; i < 45; ++i) {
        (void)h.pump(1);
        h.clock.advanceMs(100);
    }
    const auto status = h.send(1, h.request(ServiceId::ReadDataByIdentifier,
                                            uwb::protocol::bytesOf(uwb::protocol::encodeReadDidRequest(
                                                uwb::protocol::ReadDidRequest{
                                                    static_cast<std::uint16_t>(Did::ConnectionStatus)})),
                                            0xB008));
    const auto decoded =
        uwb::protocol::decodeReadDidResponse(frame::servicePdu(*findFirst(status, PayloadType::ApplicationMessage)));
    REQUIRE(decoded.ok());
    const auto record = uwb::protocol::decodeConnectionStatusRecord(decoded.value().data);
    REQUIRE(record.ok());
    CHECK(record.value().activeSession == static_cast<std::uint8_t>(uwb::protocol::SessionId::Extended));
}

TEST_CASE("a service response overtakes a queued measurement backlog", "[unit][server][tx][priority]") {
    Harness h;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Observer));

    const ByteBuffer subscribe = uwb::protocol::encodeEventSubscribeRequest(uwb::protocol::EventSubscribeRequest{
        static_cast<std::uint16_t>(EventId::UwbMeasurement),
        static_cast<std::uint8_t>(uwb::protocol::StreamMode::Live), 0, 0});
    (void)h.send(1, h.request(ServiceId::EventControl, uwb::protocol::bytesOf(subscribe), 0xC001));

    // Fill the live stream backlog (§53: bulk events are lower priority).
    for (std::uint16_t tag = 1; tag <= 4; ++tag) {
        uwb::protocol::MeasurementEvent measurement;
        measurement.networkId = 1;
        measurement.anchorId = 1;
        measurement.tagId = tag;
        measurement.rawRangeMm = 1000 + static_cast<std::int32_t>(tag);
        measurement.correctedRangeMm = 1000 + static_cast<std::int32_t>(tag);
        measurement.quality = 900;
        (void)h.core.publishMeasurement(measurement);
    }

    // A read must still be answered before the queued events (§53 priority order).
    const auto frames = h.send(1, h.request(ServiceId::ReadDataByIdentifier,
                                            uwb::protocol::bytesOf(uwb::protocol::encodeReadDidRequest(
                                                uwb::protocol::ReadDidRequest{
                                                    static_cast<std::uint16_t>(Did::EffectiveCapabilities)})),
                                            0xC002));
    REQUIRE(frame::countType(frames, PayloadType::ApplicationMessage) == 1);
    CHECK(frame::sid(*findFirst(frames, PayloadType::ApplicationMessage)) == positiveSid(ServiceId::ReadDataByIdentifier));
    CHECK(frame::countType(frames, PayloadType::EventNotification) == 4);

    // The response (and its ACK) leave before any queued measurement (§38, SRV-005).
    std::size_t responseIndex = 0;
    std::size_t firstEventIndex = frames.size();
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (frames[i].type == PayloadType::ApplicationMessage && responseIndex == 0) {
            responseIndex = i;
        }
        if (frames[i].type == PayloadType::EventNotification && firstEventIndex == frames.size()) {
            firstEventIndex = i;
        }
    }
    CHECK(responseIndex < firstEventIndex);
}

// ---------------------------------------------------------------------------
// Raw AT (specification §30)
// ---------------------------------------------------------------------------

TEST_CASE("raw AT is refused when disabled", "[unit][server][at]") {
    Harness h;
    h.config.rawAtEnabled = false;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Control));
    h.enterExtendedSession(1);

    const ByteBuffer body = uwb::protocol::encodeExecuteAtRequest(
        uwb::protocol::ExecuteAtRequest{ByteBuffer{'A', 'T', '+', 'G', 'E', 'T', 'V', 'E', 'R'}});
    const auto frames = h.send(1, h.request(ServiceId::ExecuteAtCommand, uwb::protocol::bytesOf(body)));
    CHECK(frame::nrc(*findFirst(frames, PayloadType::ApplicationMessage)) == ServiceNrc::ServiceNotSupportedInActiveSession);
    CHECK(h.backend.pendingOperations() == 0);
}

TEST_CASE("raw AT needs Control and Extended Session", "[unit][server][at][policy]") {
    Harness h;
    h.config.rawAtEnabled = true;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Control));

    const ByteBuffer body = uwb::protocol::encodeExecuteAtRequest(
        uwb::protocol::ExecuteAtRequest{ByteBuffer{'A', 'T'}});
    const auto frames = h.send(1, h.request(ServiceId::ExecuteAtCommand, uwb::protocol::bytesOf(body)));
    CHECK(frame::nrc(*findFirst(frames, PayloadType::ApplicationMessage)) == ServiceNrc::ConditionsNotCorrect);
}

TEST_CASE("raw AT answers 0x78 then the module text", "[unit][server][at][async]") {
    Harness h;
    h.config.rawAtEnabled = true;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Control));
    h.enterExtendedSession(1);

    const ByteBuffer body = uwb::protocol::encodeExecuteAtRequest(
        uwb::protocol::ExecuteAtRequest{ByteBuffer{'A', 'T', '+', 'G', 'E', 'T', 'V', 'E', 'R'}});
    const auto pending = h.send(1, h.request(ServiceId::ExecuteAtCommand, uwb::protocol::bytesOf(body)));
    CHECK(frame::nrc(*findFirst(pending, PayloadType::ApplicationMessage)) == ServiceNrc::ResponsePending);

    const FakeBackend::Submission *submission = h.backend.find(0);
    REQUIRE(submission != nullptr);
    CHECK(std::string(submission->payload.begin(), submission->payload.end()) == "AT+GETVER");
    CHECK(submission->timeoutMs == h.config.timeouts.uartCommandTimeoutMs); // §52 timeout is passed down

    h.backend.completeAt(0, AtResult{BackendStatus::Completed,
                                    ByteBuffer{'g', 'e', 't', 'v', 'e', 'r', '\r', '\n'}});
    const auto frames = h.pump(1);
    auto response = uwb::protocol::decodeExecuteAtResponse(
        frame::servicePdu(*findFirst(frames, PayloadType::ApplicationMessage)));
    REQUIRE(response.ok());
    CHECK(response.value().rawResponse.size() == 8);
}

TEST_CASE("an oversized AT command is refused", "[unit][server][at]") {
    Harness h;
    h.config.rawAtEnabled = true;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Control));
    h.enterExtendedSession(1);

    const ByteBuffer body = uwb::protocol::encodeExecuteAtRequest(
        uwb::protocol::ExecuteAtRequest{ByteBuffer(uwb::protocol::kMaxAtCommandLength + 1, 'A')});
    const auto frames = h.send(1, h.request(ServiceId::ExecuteAtCommand, uwb::protocol::bytesOf(body)));
    CHECK(frame::nrc(*findFirst(frames, PayloadType::ApplicationMessage)) == ServiceNrc::RequestOutOfRange);
    CHECK(h.backend.pendingOperations() == 0);
}

// ---------------------------------------------------------------------------
// Event Control and streaming (specification §31, §36, §37, §38)
// ---------------------------------------------------------------------------

TEST_CASE("a subscribed measurement stream delivers Event Notifications", "[unit][server][events]") {
    Harness h;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Observer));

    const ByteBuffer subscribe = uwb::protocol::encodeEventSubscribeRequest(uwb::protocol::EventSubscribeRequest{
        static_cast<std::uint16_t>(EventId::UwbMeasurement), static_cast<std::uint8_t>(uwb::protocol::StreamMode::Live),
        0});
    const auto ackFrames = h.send(1, h.request(ServiceId::EventControl, uwb::protocol::bytesOf(subscribe)));
    auto sub = uwb::protocol::decodeEventSubscribeResponse(
        frame::servicePdu(*findFirst(ackFrames, PayloadType::ApplicationMessage)));
    REQUIRE(sub.ok());
    CHECK(sub.value().queueCapacity == h.core.events().queueCapacity(uwb::protocol::StreamMode::Live));

    uwb::protocol::MeasurementEvent measurement;
    measurement.networkId = 1;
    measurement.anchorId = 2;
    measurement.tagId = 7;
    measurement.rawRangeMm = 1234;
    CHECK(h.core.publishMeasurement(measurement) == 1);

    const auto frames = h.pump(1);
    const frame::ServerFrame *note = findFirst(frames, PayloadType::EventNotification);
    REQUIRE(note != nullptr);
    auto notification = uwb::protocol::decodeEventNotification(payloadOf(*note));
    REQUIRE(notification.ok());
    CHECK(notification.value().eventId == static_cast<std::uint16_t>(EventId::UwbMeasurement));
    CHECK(notification.value().streamId == sub.value().streamId);
    CHECK(notification.value().sequence == 1);

    auto event = uwb::protocol::decodeMeasurementEvent(uwb::protocol::ConstBytes{
        notification.value().payload.data(), notification.value().payload.size()});
    REQUIRE(event.ok());
    CHECK(event.value().tagId == 7);
    CHECK(event.value().rawRangeMm == 1234);
}

TEST_CASE("unknown stream ids and sub-functions are answered with NRCs", "[unit][server][events]") {
    Harness h;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Observer));

    const ByteBuffer query = uwb::protocol::encodeEventQueryRequest(uwb::protocol::EventQueryRequest{123});
    const auto frames = h.send(1, h.request(ServiceId::EventControl, uwb::protocol::bytesOf(query)));
    CHECK(frame::nrc(*findFirst(frames, PayloadType::ApplicationMessage)) == ServiceNrc::RequestOutOfRange);

    ByteBuffer badSubFunction;
    badSubFunction.push_back(sidOf(ServiceId::EventControl));
    badSubFunction.push_back(0x77);
    const auto bad = h.send(1, frame::client(kClientA, 5, uwb::protocol::bytesOf(badSubFunction), kServerAddress));
    CHECK(frame::nrc(*findFirst(bad, PayloadType::ApplicationMessage)) == ServiceNrc::SubFunctionNotSupported);
}

TEST_CASE("live stream drops are visible in the subscription query", "[unit][server][events][live]") {
    Harness h;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Observer));

    const ByteBuffer subscribe = uwb::protocol::encodeEventSubscribeRequest(uwb::protocol::EventSubscribeRequest{
        static_cast<std::uint16_t>(EventId::UwbMeasurement), static_cast<std::uint8_t>(uwb::protocol::StreamMode::Live),
        0});
    const auto frames = h.send(1, h.request(ServiceId::EventControl, uwb::protocol::bytesOf(subscribe)));
    auto sub = uwb::protocol::decodeEventSubscribeResponse(
        frame::servicePdu(*findFirst(frames, PayloadType::ApplicationMessage)));
    REQUIRE(sub.ok());

    // Produce more samples than the connection TX queue can hold in one tick.
    uwb::protocol::MeasurementEvent measurement;
    measurement.tagId = 3;
    for (int i = 0; i < 12; ++i) {
        (void)h.core.publishMeasurement(measurement);
    }
    h.core.tick();
    const auto flushed = h.drain(1);

    const auto queryFrames = h.send(1, h.request(ServiceId::EventControl,
                                                 uwb::protocol::bytesOf(uwb::protocol::encodeEventQueryRequest(
                                                     uwb::protocol::EventQueryRequest{sub.value().streamId})),
                                                 0x55));
    auto info = uwb::protocol::decodeEventQueryResponse(
        frame::servicePdu(*findFirst(queryFrames, PayloadType::ApplicationMessage)));
    REQUIRE(info.ok());
    CHECK(info.value().state == static_cast<std::uint8_t>(uwb::protocol::StreamState::Active));
    // Nothing is silently lost: either it was sent or it is counted (§37).
    CHECK(static_cast<std::size_t>(info.value().droppedCount) + flushed.size() >= 12ULL);
}

// ---------------------------------------------------------------------------
// Device reset (specification §24)
// ---------------------------------------------------------------------------

TEST_CASE("a Control connection can request a Pico reset after the response", "[unit][server][reset]") {
    Harness h;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Control));
    h.enterExtendedSession(1);

    const ByteBuffer body = uwb::protocol::encodeDeviceResetRequest(
        uwb::protocol::DeviceResetRequest{static_cast<std::uint8_t>(uwb::protocol::DeviceResetType::Soft)});
    const auto frames = h.send(1, h.request(ServiceId::PicoDeviceReset, uwb::protocol::bytesOf(body)));

    const frame::ServerFrame *answer = findFirst(frames, PayloadType::ApplicationMessage);
    REQUIRE(answer != nullptr);
    CHECK(frame::sid(*answer) == positiveSid(ServiceId::PicoDeviceReset));

    const auto reset = h.core.takeResetRequest();
    REQUIRE(reset.has_value());
    CHECK_FALSE(reset->hardReset);
    CHECK(reset->executeAfterUs > h.clock.monotonicUs()); // §24: reset only after the response is flushed
    CHECK_FALSE(h.core.takeResetRequest().has_value()); // consumed once
}

TEST_CASE("an Observer cannot reset the device", "[unit][server][reset][policy]") {
    Harness h;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Observer));

    const ByteBuffer body = uwb::protocol::encodeDeviceResetRequest(
        uwb::protocol::DeviceResetRequest{static_cast<std::uint8_t>(uwb::protocol::DeviceResetType::Hard)});
    const auto frames = h.send(1, h.request(ServiceId::PicoDeviceReset, uwb::protocol::bytesOf(body)));
    CHECK(frame::nrc(*findFirst(frames, PayloadType::ApplicationMessage)) == ServiceNrc::ConditionsNotCorrect);
    CHECK_FALSE(h.core.takeResetRequest().has_value());
}

// ---------------------------------------------------------------------------
// Keepalive and timeouts (specification §19, §22.3, §52)
// ---------------------------------------------------------------------------

TEST_CASE("the server sends an Alive Check when a connection is silent", "[unit][server][keepalive]") {
    Harness h;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Observer));

    h.clock.advanceMs(h.config.timeouts.aliveCheckIntervalMs);
    const auto frames = h.pump(1);
    const frame::ServerFrame *check = findFirst(frames, PayloadType::AliveCheckRequest);
    REQUIRE(check != nullptr);
    auto request = uwb::protocol::decodeAliveCheckRequest(payloadOf(*check));
    REQUIRE(request.ok());

    // The client echoes the nonce; the server answers and resets its timers.
    const auto echo = h.send(1, frame::aliveCheck(request.value().nonce));
    const frame::ServerFrame *response = findFirst(echo, PayloadType::AliveCheckResponse);
    REQUIRE(response != nullptr);
    auto decoded = uwb::protocol::decodeAliveCheckResponse(payloadOf(*response));
    REQUIRE(decoded.ok());
    CHECK(decoded.value().nonce == request.value().nonce);
}

TEST_CASE("a completely silent connection is closed by the idle timeout", "[unit][server][timeout]") {
    Harness h;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Observer));

    h.clock.advanceMs(h.config.timeouts.tcpIdleTimeoutMs + 1);
    (void)h.pump(1);
    CHECK_FALSE(h.core.wantsConnectionOpen(1));
}

TEST_CASE("an idle Extended Session falls back to Default Session", "[unit][server][timeout]") {
    Harness h;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Control));
    h.enterExtendedSession(1);
    REQUIRE(h.core.connections().find(1)->isExtendedSession());

    h.clock.advanceMs(h.config.timeouts.sessionInactivityTimeoutMs + 1);
    (void)h.pump(1);

    const ConnectionContext *ctx = h.core.connections().find(1);
    REQUIRE(ctx != nullptr);
    CHECK(ctx->session == SessionState::Default); // §22.3
    CHECK(!h.logger.entries.empty());
}

// ---------------------------------------------------------------------------
// Disconnect cleanup (specification §9.4)
// ---------------------------------------------------------------------------

TEST_CASE("disconnect cancels pending work, routines and streams of that connection",
          "[unit][server][disconnect]") {
    Harness h;
    h.config.rawAtEnabled = true;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Control));
    h.enterExtendedSession(1);

    const ByteBuffer at = uwb::protocol::encodeExecuteAtRequest(uwb::protocol::ExecuteAtRequest{ByteBuffer{'A', 'T'}});
    (void)h.send(1, h.request(ServiceId::ExecuteAtCommand, uwb::protocol::bytesOf(at)));
    CHECK(h.core.pendingRequestCount() == 1);

    const ByteBuffer subscribe = uwb::protocol::encodeEventSubscribeRequest(uwb::protocol::EventSubscribeRequest{
        static_cast<std::uint16_t>(EventId::UwbMeasurement), static_cast<std::uint8_t>(uwb::protocol::StreamMode::Live),
        0});
    (void)h.send(1, h.request(ServiceId::EventControl, uwb::protocol::bytesOf(subscribe)));
    CHECK(h.core.events().streamCount(1) == 1);

    const ByteBuffer routine = uwb::protocol::encodeRoutineControlRequest(uwb::protocol::RoutineControlRequest{
        static_cast<std::uint8_t>(uwb::protocol::RoutineControlType::Start),
        static_cast<std::uint16_t>(RoutineId::UwbMeasurementAcquisition), {}});
    (void)h.send(1, h.request(ServiceId::RoutineControl, uwb::protocol::bytesOf(routine), 0x77));
    CHECK(h.core.routines().runningCount() == 1);

    h.core.onDisconnect(1);

    CHECK(h.core.pendingRequestCount() == 0);
    CHECK(h.core.events().streamCount(1) == 0);
    CHECK(h.core.routines().runningCount() == 0);
    CHECK(h.security.resetCalls_ >= 1);
    for (const FakeBackend::Submission &submission : h.backend.submissions_) {
        CHECK(submission.cancelled); // §9.4: the UWB channel is released
    }
    CHECK(h.core.connectionCount() == 0);
}

TEST_CASE("a stalled peer keeps its frames queued instead of losing them", "[unit][server][tx]") {
    Harness h;
    h.connect(1);
    REQUIRE(h.activate(1, ConnectionRole::Observer));

    FakeConnectionWriter &writer = h.writers[1];
    writer.resetTickLimit(0); // transport reports "cannot accept anything now"

    const ByteBuffer first = h.clientPresent(0xA1);
    const ByteBuffer second = h.clientPresent(0xA2);
    (void)h.send(1, first);
    (void)h.send(1, second);

    CHECK(writer.frameCount() == 0);

    writer.resetTickLimit(100); // transport becomes ready again
    const auto frames = h.pump(1);
    CHECK(frame::countType(frames, PayloadType::ApplicationMessage) >= 2);
}
