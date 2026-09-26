#include <array>
#include <cstdint>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "uwb/protocol/generic_header.hpp"
#include "uwb/protocol/payload_types.hpp"
#include "uwb/protocol/payloads.hpp"

#include "gen/golden_vectors.hpp"
#include "test_helpers.hpp"

using namespace uwb::protocol;
using uwb::test::sameBytes;
using uwb::test::view;

namespace {

Uuid uuidOf(const std::array<std::uint8_t, 16> &raw) noexcept {
    Uuid uuid;
    uuid.bytes = raw;
    return uuid;
}

} // namespace

TEST_CASE("Generic Header NACK golden (§15)", "[protocol][payload][golden]") {
    auto nack = decodeGenericHeaderNack(view(uwb::test::kGenericHeaderNackUnknownType));
    REQUIRE(nack.ok());
    CHECK(nack->code == GenericHeaderNackCode::UnknownPayloadType);

    auto tooLarge = decodeGenericHeaderNack(view(uwb::test::kGenericHeaderNackTooLarge));
    REQUIRE(tooLarge.ok());
    CHECK(tooLarge->code == GenericHeaderNackCode::MessageTooLarge);

    ByteBuffer encoded = encodeGenericHeaderNack(GenericHeaderNack{GenericHeaderNackCode::UnknownPayloadType});
    CHECK(sameBytes(bytesOf(encoded), view(uwb::test::kGenericHeaderNackUnknownType)));

    // §15: exactly one byte, value 0x00..0x06.
    const std::array<std::uint8_t, 1> invalidCode = {0x07};
    CHECK(decodeGenericHeaderNack(view(invalidCode)).failed());
    CHECK(decodeGenericHeaderNack(ConstBytes{}).failed());
}

TEST_CASE("Device ID request is zero length (§16)", "[protocol][payload]") {
    CHECK(encodeDeviceIdRequest({}).empty());
    CHECK(decodeDeviceIdRequest(ConstBytes{}).ok());
    const std::array<std::uint8_t, 1> junk = {0x00};
    CHECK(decodeDeviceIdRequest(view(junk)).failed());
}

TEST_CASE("Device ID response golden (§17)", "[protocol][payload][golden]") {
    auto decoded = decodeDeviceIdResponse(view(uwb::test::kDeviceIdResponsePayload));
    REQUIRE(decoded.ok());
    CHECK(decoded->deviceUuid.bytes == uwb::test::kDeviceUuid);
    CHECK(decoded->logicalAddress == 0x1000);
    CHECK(decoded->tcpPort == 13401);
    CHECK(decoded->capabilityMask == 0x000000000003FFFFULL);
    CHECK(decoded->controlStatus == ControlStatus::Available);
    CHECK(decoded->activeObservers == 1);
    CHECK(decoded->maxObservers == 4);
    CHECK(decoded->deviceName == "pico-anchor");

    ByteBuffer encoded = encodeDeviceIdResponse(decoded.value());
    CHECK(sameBytes(bytesOf(encoded), view(uwb::test::kDeviceIdResponsePayload)));
    CHECK(encoded.size() == uwb::test::kDeviceIdResponsePayload.size());
}

TEST_CASE("Device ID response rejects malformed payloads (§17)", "[protocol][payload]") {
    ConstBytes full = view(uwb::test::kDeviceIdResponsePayload);

    // Truncated.
    CHECK(decodeDeviceIdResponse(full.subspan(0, full.size() - 1)).failed());
    // Trailing byte.
    ByteBuffer extra{full.begin(), full.end()};
    extra.push_back(0x00);
    CHECK(decodeDeviceIdResponse(bytesOf(extra)).failed());

    // Device name length beyond the §40.3 limit of 32.
    ByteBuffer tooLongName{full.begin(), full.begin() + 31};
    tooLongName.push_back(33);
    auto bad = decodeDeviceIdResponse(bytesOf(tooLongName));
    REQUIRE(bad.failed());
    CHECK(bad.code() == ProtocolErrorCode::InvalidField);

    // Empty device name is allowed.
    ByteBuffer emptyName{full.begin(), full.begin() + 31};
    emptyName.push_back(0);
    auto nameless = decodeDeviceIdResponse(bytesOf(emptyName));
    REQUIRE(nameless.ok());
    CHECK(nameless->deviceName.empty());

    // Occupied control status.
    ByteBuffer occupied{full.begin(), full.end()};
    occupied[28] = 0x01;
    auto decoded = decodeDeviceIdResponse(bytesOf(occupied));
    REQUIRE(decoded.ok());
    CHECK(decoded->controlStatus == ControlStatus::Occupied);
}

TEST_CASE("Connection activation golden (§18)", "[protocol][payload][golden]") {
    auto request = decodeConnectionActivationRequest(view(uwb::test::kActivationRequestPayload));
    REQUIRE(request.ok());
    CHECK(request->clientLogicalAddress == 0x0E00);
    CHECK(request->requestedRole == ConnectionRole::Control);
    CHECK(request->flags == 0);
    CHECK(request->clientInstanceUuid.bytes == uwb::test::kClientUuid);

    auto response = decodeConnectionActivationResponse(view(uwb::test::kActivationResponsePayload));
    REQUIRE(response.ok());
    CHECK(response->serverLogicalAddress == 0x1000);
    CHECK(response->assignedRole == ConnectionRole::Control);
    CHECK(response->responseCode == ActivationResponseCode::AcceptedRequestedRole);
    CHECK(response->deviceUuid.bytes == uwb::test::kDeviceUuid);
    CHECK(response->serverMaxPayload == 4096U);
    CHECK(response->capabilityMask == 0x000000000003FFFFULL);

    CHECK(sameBytes(bytesOf(encodeConnectionActivationRequest(request.value())),
                          view(uwb::test::kActivationRequestPayload)));
    CHECK(sameBytes(bytesOf(encodeConnectionActivationResponse(response.value())),
                          view(uwb::test::kActivationResponsePayload)));
}

TEST_CASE("Connection activation rejects invalid fields (§18)", "[protocol][payload]") {
    ByteBuffer request{uwb::test::kActivationRequestPayload.begin(), uwb::test::kActivationRequestPayload.end()};

    request[3] = 0x01; // flags must be zero in v1
    CHECK(decodeConnectionActivationRequest(bytesOf(request)).failed());
    request[3] = 0x00;

    request[2] = 0x03; // unknown role
    CHECK(decodeConnectionActivationRequest(bytesOf(request)).failed());
    request[2] = 0x01;

    // Address-range policy is enforced by the server (specification §20.3), not by
    // the codec; the codec only validates size, role and flags.
    CHECK(decodeConnectionActivationRequest(ConstBytes{}).failed());
    CHECK(decodeConnectionActivationRequest(ConstBytes{request.data(), 19}).failed());
}

TEST_CASE("Activation rejection responses keep assigned role zero (§18.2)", "[protocol][payload]") {
    ConnectionActivationResponse rejection;
    rejection.serverLogicalAddress = 0x1000;
    rejection.assignedRole = ConnectionRole::None;
    rejection.responseCode = ActivationResponseCode::RejectedRolePolicyDenied;
    rejection.deviceUuid = uuidOf(uwb::test::kDeviceUuid);
    rejection.serverMaxPayload = 4096;
    rejection.capabilityMask = 0x000000000001FFFFULL;

    ByteBuffer bytes = encodeConnectionActivationResponse(rejection);
    auto decoded = decodeConnectionActivationResponse(bytesOf(bytes));
    REQUIRE(decoded.ok());
    CHECK(decoded->assignedRole == ConnectionRole::None);
    CHECK(decoded->responseCode == ActivationResponseCode::RejectedRolePolicyDenied);
    CHECK(isActivationRejection(decoded->responseCode));
}

TEST_CASE("Alive check echoes the nonce (§19)", "[protocol][payload][golden]") {
    auto request = decodeAliveCheckRequest(view(uwb::test::kAliveCheckPayload));
    REQUIRE(request.ok());
    CHECK(request->nonce == 0x0123456789ABCDEFULL);

    ByteBuffer encoded = encodeAliveCheckResponse(AliveCheckResponse{request->nonce});
    CHECK(sameBytes(bytesOf(encoded), view(uwb::test::kAliveCheckPayload)));

    CHECK(decodeAliveCheckRequest(ConstBytes{}).failed());
    const std::array<std::uint8_t, 7> truncated = {1, 2, 3, 4, 5, 6, 7};
    CHECK(decodeAliveCheckRequest(view(truncated)).failed());
}

TEST_CASE("Application envelope golden (§20.1)", "[protocol][payload][golden]") {
    auto envelope = decodeApplicationEnvelope(view(uwb::test::kApplicationEnvelopeReadUuid));
    REQUIRE(envelope.ok());
    CHECK(envelope->sourceLogicalAddress == 0x0E00);
    CHECK(envelope->targetLogicalAddress == 0x1000);
    CHECK(envelope->transactionId == 0x42U);
    CHECK(envelope->flags == 0);
    CHECK(envelope->ackRequired() == false);
    REQUIRE(envelope->servicePdu.size() == 3);
    CHECK(envelope->servicePdu[0] == 0x22);
    CHECK(envelope->servicePdu[1] == 0xF0);
    CHECK(envelope->servicePdu[2] == 0x00);

    CHECK(sameBytes(bytesOf(encodeApplicationEnvelope(envelope.value())),
                          view(uwb::test::kApplicationEnvelopeReadUuid)));

    auto ackRequired = decodeApplicationEnvelope(view(uwb::test::kApplicationEnvelopeAckRequired));
    REQUIRE(ackRequired.ok());
    CHECK(ackRequired->ackRequired());

    // Reserved bits must be zero.
    ByteBuffer bad{uwb::test::kApplicationEnvelopeReadUuid.begin(), uwb::test::kApplicationEnvelopeReadUuid.end()};
    bad[11] = 0x01;
    CHECK(decodeApplicationEnvelope(bytesOf(bad)).failed());

    // Reserved flags bits (1..7) must be zero.
    ByteBuffer badFlags = bad;
    badFlags[11] = 0x00;
    badFlags[9] = 0x02;
    CHECK(decodeApplicationEnvelope(bytesOf(badFlags)).failed());

    // Envelope shorter than the fixed part.
    CHECK(decodeApplicationEnvelope(ConstBytes{}).failed());
    ByteBuffer short10{bad.begin(), bad.begin() + 11};
    CHECK(decodeApplicationEnvelope(bytesOf(short10)).failed());
}

TEST_CASE("Application ACK and NACK golden (§20.2, §20.3)", "[protocol][payload][golden]") {
    auto ack = decodeApplicationMessageAck(view(uwb::test::kApplicationAckPayload));
    REQUIRE(ack.ok());
    CHECK(ack->sourceLogicalAddress == 0x1000);
    CHECK(ack->targetLogicalAddress == 0x0E00);
    CHECK(ack->transactionId == 0x42U);
    CHECK(sameBytes(bytesOf(encodeApplicationMessageAck(ack.value())), view(uwb::test::kApplicationAckPayload)));

    auto nack = decodeApplicationMessageNack(view(uwb::test::kApplicationNackPayload));
    REQUIRE(nack.ok());
    CHECK(nack->code == ApplicationNackCode::InvalidSourceOrTargetAddress);
    CHECK(sameBytes(bytesOf(encodeApplicationMessageNack(nack.value())), view(uwb::test::kApplicationNackPayload)));

    const std::array<std::uint8_t, 9> badCode = {0x10, 0x00, 0x0E, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00};
    CHECK(decodeApplicationMessageNack(view(badCode)).failed());
}

TEST_CASE("Frames round-trip through the frame helpers", "[protocol][payload][golden]") {
    DeviceIdResponse response;
    response.deviceUuid = uuidOf(uwb::test::kDeviceUuid);
    response.logicalAddress = 0x1000;
    response.tcpPort = 13401;
    response.capabilityMask = 0x000000000003FFFFULL;
    response.controlStatus = ControlStatus::Available;
    response.activeObservers = 1;
    response.maxObservers = 4;
    response.deviceName = "pico-anchor";

    auto frame = encodeFrame(PayloadType::DeviceIdResponse, bytesOf(encodeDeviceIdResponse(response)));
    REQUIRE(frame.ok());
    CHECK(sameBytes(bytesOf(frame.value()), view(uwb::test::kDeviceIdResponseFrame)));
}
