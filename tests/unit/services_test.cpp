#include <array>
#include <cstdint>
#include <string>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "uwb/protocol/constants.hpp"
#include "uwb/protocol/did_records.hpp"
#include "uwb/protocol/events.hpp"
#include "uwb/protocol/routines.hpp"
#include "uwb/protocol/service_ids.hpp"
#include "uwb/protocol/services.hpp"

#include "gen/golden_vectors.hpp"
#include "test_helpers.hpp"

using namespace uwb::protocol;
using uwb::test::sameBytes;
using uwb::test::view;

TEST_CASE("Service PDU framing rules (§21)", "[protocol][service]") {
    auto pdu = decodeServicePdu(view(uwb::test::kReadDidDeviceNameRequest));
    REQUIRE(pdu.ok());
    CHECK(pdu->sid == 0x22);
    CHECK_FALSE(pdu->negative());
    CHECK(pdu->positiveSid() == 0x62);
    CHECK(sameBytes(bytesOf(encodeServicePdu(pdu.value())), view(uwb::test::kReadDidDeviceNameRequest)));

    CHECK(decodeServicePdu(ConstBytes{}).code() == ProtocolErrorCode::EmptyServicePdu);

    CHECK(isPositiveResponseFor(0x22, view(uwb::test::kReadDidDeviceNameResponse)));
    CHECK_FALSE(isPositiveResponseFor(0x22, view(uwb::test::kNegativeResponseRequestOutOfRange)));
    CHECK_FALSE(isPositiveResponseFor(0x2E, view(uwb::test::kReadDidDeviceNameResponse)));
}

TEST_CASE("Negative service response golden (§21.3)", "[protocol][service][golden]") {
    auto negative = decodeNegativeResponse(view(uwb::test::kNegativeResponseRequestOutOfRange));
    REQUIRE(negative.ok());
    CHECK(negative->first == 0x22);
    CHECK(negative->second == ServiceNrc::RequestOutOfRange);

    CHECK(sameBytes(bytesOf(encodeNegativeResponse(0x22, ServiceNrc::RequestOutOfRange)),
                          view(uwb::test::kNegativeResponseRequestOutOfRange)));
    CHECK(sameBytes(bytesOf(encodeNegativeResponse(0x10, ServiceNrc::ServiceNotSupportedInActiveSession)),
                          view(uwb::test::kNegativeResponseSession)));

    const std::array<std::uint8_t, 2> truncated = {0x7F, 0x22};
    CHECK(decodeNegativeResponse(view(truncated)).failed());
}

TEST_CASE("Session Control golden (§23)", "[protocol][service][golden]") {
    auto request = decodeSessionControlRequest(view(uwb::test::kSessionControlRequestExtended));
    REQUIRE(request.ok());
    CHECK(request->requestedSession == static_cast<std::uint8_t>(SessionId::Extended));
    CHECK(sameBytes(bytesOf(encodeSessionControlRequest(request.value())),
                          view(uwb::test::kSessionControlRequestExtended)));

    auto defaultSession = decodeSessionControlRequest(view(uwb::test::kSessionControlRequestDefault));
    REQUIRE(defaultSession.ok());
    CHECK(defaultSession->requestedSession == static_cast<std::uint8_t>(SessionId::Default));

    auto response = decodeSessionControlResponse(view(uwb::test::kSessionControlResponseExtended));
    REQUIRE(response.ok());
    CHECK(response->activeSession == static_cast<std::uint8_t>(SessionId::Extended));
    CHECK(response->p2ServerMaxMs == 1000);
    CHECK(response->p2StarServerMax10ms == 500); // 5000 ms in 10 ms units (§23)
    CHECK(sameBytes(bytesOf(encodeSessionControlResponse(response.value())),
                          view(uwb::test::kSessionControlResponseExtended)));

    const std::array<std::uint8_t, 2> unknownSession = {0x10, 0x07};
    CHECK(decodeSessionControlRequest(view(unknownSession)).failed());
    CHECK(decodeSessionControlRequest(ConstBytes{}).failed());
}

TEST_CASE("Device Reset golden (§24)", "[protocol][service][golden]") {
    auto soft = decodeDeviceResetRequest(view(uwb::test::kDeviceResetRequestSoft));
    REQUIRE(soft.ok());
    CHECK(soft->resetType == static_cast<std::uint8_t>(DeviceResetType::Soft));
    CHECK(sameBytes(bytesOf(encodeDeviceResetRequest(soft.value())), view(uwb::test::kDeviceResetRequestSoft)));
    CHECK(sameBytes(bytesOf(encodeDeviceResetResponse(DeviceResetResponse{static_cast<std::uint8_t>(DeviceResetType::Soft)})),
                          view(uwb::test::kDeviceResetResponseSoft)));

    auto hard = decodeDeviceResetRequest(view(uwb::test::kDeviceResetRequestHard));
    REQUIRE(hard.ok());
    CHECK(hard->resetType == static_cast<std::uint8_t>(DeviceResetType::Hard));

    const std::array<std::uint8_t, 2> invalid = {0x11, 0x02};
    CHECK(decodeDeviceResetRequest(view(invalid)).failed());
}

TEST_CASE("Security Access golden (§25)", "[protocol][service][golden]") {
    auto seedRequest = decodeSecurityAccessRequest(view(uwb::test::kSecurityRequestSeed));
    REQUIRE(seedRequest.ok());
    CHECK(seedRequest->subFunction == static_cast<std::uint8_t>(SecuritySubFunction::RequestSeed));

    auto seedResponse = decodeSecurityAccessResponse(view(uwb::test::kSecurityResponseSeed));
    REQUIRE(seedResponse.ok());
    CHECK(seedResponse->subFunction == static_cast<std::uint8_t>(SecuritySubFunction::RequestSeed));
    CHECK(seedResponse->seed == 0xDEADBEEFU);
    CHECK(sameBytes(bytesOf(encodeSecurityAccessResponse(seedResponse.value())),
                          view(uwb::test::kSecurityResponseSeed)));

    auto keyRequest = decodeSecurityAccessRequest(view(uwb::test::kSecuritySendKey));
    REQUIRE(keyRequest.ok());
    CHECK(keyRequest->subFunction == static_cast<std::uint8_t>(SecuritySubFunction::SendKey));
    CHECK(keyRequest->key == 0xCAFEBABEU);
    CHECK(sameBytes(bytesOf(encodeSecurityAccessRequest(keyRequest.value())), view(uwb::test::kSecuritySendKey)));

    auto keyResponse = decodeSecurityAccessResponse(view(uwb::test::kSecurityResponseKeyOk));
    REQUIRE(keyResponse.ok());
    CHECK(keyResponse->subFunction == static_cast<std::uint8_t>(SecuritySubFunction::SendKey));

    // Only 0x01 and 0x02 are defined.
    const std::array<std::uint8_t, 2> badSub = {0x27, 0x03};
    CHECK(decodeSecurityAccessRequest(view(badSub)).failed());
    // A key request must carry the 4-byte key.
    CHECK(decodeSecurityAccessRequest(bytesOf(std::array<std::uint8_t, 2>{0x27, 0x02})).failed());
    // A seed response must carry the 4-byte seed.
    CHECK(decodeSecurityAccessResponse(bytesOf(std::array<std::uint8_t, 2>{0x67, 0x01})).failed());
}

TEST_CASE("Read / Write DID golden (§26, §27, §40.3)", "[protocol][service][golden]") {
    auto readRequest = decodeReadDidRequest(view(uwb::test::kReadDidDeviceNameRequest));
    REQUIRE(readRequest.ok());
    CHECK(readRequest->did == 0xF002);
    CHECK(sameBytes(bytesOf(encodeReadDidRequest(readRequest.value())), view(uwb::test::kReadDidDeviceNameRequest)));

    auto readResponse = decodeReadDidResponse(view(uwb::test::kReadDidDeviceNameResponse));
    REQUIRE(readResponse.ok());
    CHECK(readResponse->did == 0xF002);
    CHECK(readResponse->data.size() == 12); // u8 length + "pico-anchor"
    CHECK(sameBytes(bytesOf(encodeReadDidResponse(readResponse.value())),
                          view(uwb::test::kReadDidDeviceNameResponse)));

    auto writeRequest = decodeWriteDidRequest(view(uwb::test::kWriteDidDeviceNameRequest));
    REQUIRE(writeRequest.ok());
    CHECK(writeRequest->did == 0xF002);
    CHECK(writeRequest->data.size() == 12);
    CHECK(sameBytes(bytesOf(encodeWriteDidRequest(writeRequest.value())),
                          view(uwb::test::kWriteDidDeviceNameRequest)));

    auto writeResponse = decodeWriteDidResponse(view(uwb::test::kWriteDidDeviceNameResponse));
    REQUIRE(writeResponse.ok());
    CHECK(writeResponse->did == 0xF002);

    CHECK(decodeReadDidRequest(ConstBytes{view(uwb::test::kReadDidDeviceNameRequest).data(), 1}).failed());
    // Truncated DID field.
    CHECK(decodeWriteDidRequest(ConstBytes{view(uwb::test::kWriteDidDeviceNameRequest).data(), 2}).failed());
    CHECK(decodeWriteDidRequest(ConstBytes{}).failed());

    // An empty record is structurally valid here (§27); rejecting it against the
    // §40.3 record layout is DID-record / server-side validation.
    const std::array<std::uint8_t, 3> emptyRecord = {0x2E, 0xF0, 0x02};
    auto empty = decodeWriteDidRequest(view(emptyRecord));
    REQUIRE(empty.ok());
    CHECK(empty->data.empty());
    CHECK(decodeTextRecord8(empty.value().dataBytes(), 32).failed());
}

TEST_CASE("Routine Control golden (§28)", "[protocol][service][golden]") {
    auto start = decodeRoutineControlRequest(view(uwb::test::kRoutineStartMeasurement));
    REQUIRE(start.ok());
    CHECK(start->controlType == static_cast<std::uint8_t>(RoutineControlType::Start));
    CHECK(start->routineId == static_cast<std::uint16_t>(RoutineId::UwbMeasurementAcquisition));
    CHECK(start->optionRecord.empty());
    CHECK(sameBytes(bytesOf(encodeRoutineControlRequest(start.value())),
                          view(uwb::test::kRoutineStartMeasurement)));

    auto startResponse = decodeRoutineControlResponse(view(uwb::test::kRoutineStartMeasurementResponse));
    REQUIRE(startResponse.ok());
    CHECK(startResponse->routineState == static_cast<std::uint8_t>(RoutineState::Running));
    CHECK(startResponse->statusRecord.empty());

    auto results = decodeRoutineControlRequest(view(uwb::test::kRoutineResultsMeasurement));
    REQUIRE(results.ok());
    CHECK(results->controlType == static_cast<std::uint8_t>(RoutineControlType::RequestResults));

    auto resultsResponse = decodeRoutineControlResponse(view(uwb::test::kRoutineResultsMeasurementResponse));
    REQUIRE(resultsResponse.ok());
    CHECK(resultsResponse->routineState == static_cast<std::uint8_t>(RoutineState::Completed));
    REQUIRE(resultsResponse->statusRecord.size() == 2);
    CHECK(sameBytes(bytesOf(encodeRoutineControlResponse(resultsResponse.value())),
                          view(uwb::test::kRoutineResultsMeasurementResponse)));

    const std::array<std::uint8_t, 4> badControl = {0x31, 0x05, 0x02, 0x30};
    CHECK(decodeRoutineControlRequest(view(badControl)).failed());
    CHECK(decodeRoutineControlRequest(ConstBytes{}).failed());

    CHECK(isKnownRoutine(0x0230));
    CHECK_FALSE(isKnownRoutine(0x02FF));
    CHECK(routineIsCancellable(RoutineId::UwbMeasurementAcquisition));
    CHECK_FALSE(routineIsCancellable(RoutineId::SaveUwbConfiguration));
    CHECK(std::string{routineName(RoutineId::UwbMeasurementAcquisition)} == "UwbMeasurementAcquisition");
}

TEST_CASE("Client Present golden (§29)", "[protocol][service][golden]") {
    auto request = decodeClientPresentRequest(view(uwb::test::kClientPresentRequest));
    REQUIRE(request.ok());
    CHECK(request->subFunction == 0x00);
    CHECK(sameBytes(bytesOf(encodeClientPresentRequest()), view(uwb::test::kClientPresentRequest)));
    CHECK(sameBytes(bytesOf(encodeClientPresentResponse()), view(uwb::test::kClientPresentResponse)));

    const std::array<std::uint8_t, 2> badSub = {0x3E, 0x01};
    CHECK(decodeClientPresentRequest(view(badSub)).failed());
}

TEST_CASE("Execute AT Command golden (§30, AT manual V1.0.7)", "[protocol][service][golden]") {
    auto request = decodeExecuteAtRequest(view(uwb::test::kExecuteAtRequestGetVer));
    REQUIRE(request.ok());
    REQUIRE(request->command.size() == 9);
    CHECK(std::string(request->command.begin(), request->command.end()) == "AT+GETVER");
    CHECK(sameBytes(bytesOf(encodeExecuteAtRequest(request.value())), view(uwb::test::kExecuteAtRequestGetVer)));

    auto response = decodeExecuteAtResponse(view(uwb::test::kExecuteAtResponseGetVer));
    REQUIRE(response.ok());
    // Raw AT text, including CRLF, is preserved verbatim.
    CHECK(std::string(response->rawResponse.begin(), response->rawResponse.end()) ==
          "getver software:V1.0.0\r\nOK\r\n");
    CHECK(sameBytes(bytesOf(encodeExecuteAtResponse(response.value())),
                          view(uwb::test::kExecuteAtResponseGetVer)));

    // §30: commands carry no CR/LF and are limited to 512 bytes.
    ByteBuffer withCr = {0x40, 0x00, 0x02, 'A', '\r'};
    CHECK(decodeExecuteAtRequest(bytesOf(withCr)).failed());
    ByteBuffer withLf = {0x40, 0x00, 0x02, 'A', '\n'};
    CHECK(decodeExecuteAtRequest(bytesOf(withLf)).failed());

    ByteBuffer oversizedCommand(kMaxAtCommandLength + 1, 'A');
    ByteBuffer oversizedPdu = encodeExecuteAtRequest(ExecuteAtRequest{oversizedCommand});
    CHECK(oversizedPdu.size() == 3 + kMaxAtCommandLength + 1); // the writer does not validate
    auto rejected = decodeExecuteAtRequest(bytesOf(oversizedPdu));
    REQUIRE(rejected.failed());
    CHECK(rejected.code() == ProtocolErrorCode::InvalidField);

    // §30: empty command is invalid.
    const std::array<std::uint8_t, 3> empty = {0x40, 0x00, 0x00};
    CHECK(decodeExecuteAtRequest(view(empty)).failed());
}

TEST_CASE("Event Control golden (§31)", "[protocol][service][golden]") {
    auto subscribe = decodeEventSubscribeRequest(view(uwb::test::kEventSubscribeRequest));
    REQUIRE(subscribe.ok());
    CHECK(subscribe->eventId == static_cast<std::uint16_t>(EventId::UwbMeasurement));
    CHECK(subscribe->mode == static_cast<std::uint8_t>(StreamMode::Live));
    CHECK(subscribe->flags == 0);
    CHECK(subscribe->requestedPeriodMs == 100);
    CHECK(sameBytes(bytesOf(encodeEventSubscribeRequest(subscribe.value())),
                          view(uwb::test::kEventSubscribeRequest)));

    auto subscribeResponse = decodeEventSubscribeResponse(view(uwb::test::kEventSubscribeResponse));
    REQUIRE(subscribeResponse.ok());
    CHECK(subscribeResponse->streamId == 1);
    CHECK(subscribeResponse->acceptedMode == static_cast<std::uint8_t>(StreamMode::Live));
    CHECK(subscribeResponse->acceptedPeriodMs == 100);
    CHECK(subscribeResponse->queueCapacity == 4);
    CHECK(sameBytes(bytesOf(encodeEventSubscribeResponse(subscribeResponse.value())),
                          view(uwb::test::kEventSubscribeResponse)));

    auto unsubscribe = decodeEventUnsubscribeRequest(view(uwb::test::kEventUnsubscribeRequest));
    REQUIRE(unsubscribe.ok());
    CHECK(unsubscribe->streamId == 1);
    CHECK(sameBytes(bytesOf(encodeEventUnsubscribeRequest(unsubscribe.value())),
                          view(uwb::test::kEventUnsubscribeRequest)));
    CHECK(sameBytes(bytesOf(encodeEventUnsubscribeResponse({1})), view(uwb::test::kEventUnsubscribeResponse)));

    auto query = decodeEventQueryRequest(view(uwb::test::kEventQueryRequest));
    REQUIRE(query.ok());
    CHECK(query->streamId == 1);

    auto queryResponse = decodeEventQueryResponse(view(uwb::test::kEventQueryResponse));
    REQUIRE(queryResponse.ok());
    CHECK(queryResponse->eventId == static_cast<std::uint16_t>(EventId::UwbMeasurement));
    CHECK(queryResponse->state == static_cast<std::uint8_t>(StreamState::Active));
    CHECK(queryResponse->droppedCount == 3);
    CHECK(sameBytes(bytesOf(encodeEventQueryResponse(queryResponse.value())),
                          view(uwb::test::kEventQueryResponse)));

    auto unsubAll = decodeEventUnsubscribeAllRequest(view(uwb::test::kEventUnsubscribeAllRequest));
    REQUIRE(unsubAll.ok());
    CHECK(*unsubAll);
    CHECK(sameBytes(bytesOf(encodeEventUnsubscribeAllRequest()), view(uwb::test::kEventUnsubscribeAllRequest)));
    CHECK(sameBytes(bytesOf(encodeEventUnsubscribeAllResponse()),
                          view(uwb::test::kEventUnsubscribeAllResponse)));
}

TEST_CASE("Event Control rejects invalid sub-functions and modes (§31)", "[protocol][service]") {
    const std::array<std::uint8_t, 8> unknownEvent = {0x41, 0x01, 0x00, 0x99, 0x01, 0x00, 0x00, 0x64};
    CHECK(decodeEventSubscribeRequest(view(unknownEvent)).failed());

    const std::array<std::uint8_t, 8> unknownMode = {0x41, 0x01, 0x00, 0x01, 0x03, 0x00, 0x00, 0x64};
    CHECK(decodeEventSubscribeRequest(view(unknownMode)).failed());

    const std::array<std::uint8_t, 7> truncated = {0x41, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00};
    CHECK(decodeEventSubscribeRequest(view(truncated)).failed());

    const std::array<std::uint8_t, 2> badSub = {0x41, 0x09};
    CHECK(decodeEventUnsubscribeRequest(view(badSub)).failed());

    CHECK_FALSE(isKnownEventId(0x0006));
    CHECK(isKnownEventId(0x00F0));
    CHECK(isKnownStreamMode(0x01));
    CHECK(isKnownStreamMode(0x02));
    CHECK_FALSE(isKnownStreamMode(0x03));
}
