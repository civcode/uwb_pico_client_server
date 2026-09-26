#pragma once

#include <cstdint>
#include <string>

#include "uwb/protocol/bytes.hpp"
#include "uwb/protocol/constants.hpp"
#include "uwb/protocol/dids.hpp"
#include "uwb/protocol/result.hpp"
#include "uwb/protocol/routines.hpp"
#include "uwb/protocol/service_ids.hpp"

namespace uwb::protocol {

// ---------------------------------------------------------------------------
// Generic service PDU view (specification §21)
// ---------------------------------------------------------------------------
struct ServicePdu {
    std::uint8_t sid = 0;
    ByteBuffer body; // everything after the SID byte (for a negative response:
                     // request SID + NRC)

    [[nodiscard]] bool negative() const noexcept { return sid == kNegativeResponseSid; }
    [[nodiscard]] ConstBytes bodyBytes() const noexcept { return bytesOf(body); }
    [[nodiscard]] std::uint8_t positiveSid() const noexcept { return positiveResponseSid(sid); }
};

[[nodiscard]] Result<ServicePdu> decodeServicePdu(ConstBytes pdu) noexcept;
[[nodiscard]] ByteBuffer encodeServicePdu(const ServicePdu &pdu) noexcept;

// 0x7F | request SID | NRC
[[nodiscard]] ByteBuffer encodeNegativeResponse(std::uint8_t requestSid, ServiceNrc nrc) noexcept;

// Extract (requestSid, nrc) from a negative response PDU.
[[nodiscard]] Result<std::pair<std::uint8_t, ServiceNrc>> decodeNegativeResponse(ConstBytes pdu) noexcept;

[[nodiscard]] bool isPositiveResponseFor(std::uint8_t requestSid, ConstBytes pdu) noexcept;

// ---------------------------------------------------------------------------
// 0x10 Session Control (specification §23)
// ---------------------------------------------------------------------------
struct SessionControlRequest {
    std::uint8_t requestedSession = static_cast<std::uint8_t>(SessionId::Default);
};

struct SessionControlResponse {
    std::uint8_t activeSession = static_cast<std::uint8_t>(SessionId::Default);
    std::uint16_t p2ServerMaxMs = kDefaultP2ServerMaxMs;
    std::uint16_t p2StarServerMax10ms = kDefaultP2StarServerMax10ms;
};

[[nodiscard]] Result<SessionControlRequest> decodeSessionControlRequest(ConstBytes pdu) noexcept;
[[nodiscard]] ByteBuffer encodeSessionControlRequest(const SessionControlRequest &value) noexcept;
[[nodiscard]] Result<SessionControlResponse> decodeSessionControlResponse(ConstBytes pdu) noexcept;
[[nodiscard]] ByteBuffer encodeSessionControlResponse(const SessionControlResponse &value) noexcept;

// ---------------------------------------------------------------------------
// 0x11 Pico Device Reset (specification §24)
// ---------------------------------------------------------------------------
struct DeviceResetRequest {
    std::uint8_t resetType = static_cast<std::uint8_t>(DeviceResetType::Soft);
};

struct DeviceResetResponse {
    std::uint8_t resetType = static_cast<std::uint8_t>(DeviceResetType::Soft);
};

[[nodiscard]] Result<DeviceResetRequest> decodeDeviceResetRequest(ConstBytes pdu) noexcept;
[[nodiscard]] ByteBuffer encodeDeviceResetRequest(const DeviceResetRequest &value) noexcept;
[[nodiscard]] Result<DeviceResetResponse> decodeDeviceResetResponse(ConstBytes pdu) noexcept;
[[nodiscard]] ByteBuffer encodeDeviceResetResponse(const DeviceResetResponse &value) noexcept;

// ---------------------------------------------------------------------------
// 0x27 Security Access (specification §25)
//
// v1 record sizes (documented in docs/protocol_decisions.md):
//   request seed : 0x27 0x01
//   send key     : 0x27 0x02 | key:u32
//   seed response: 0x67 0x01 | seed:u32
//   key response : 0x67 0x02
// ---------------------------------------------------------------------------
inline constexpr std::size_t kSecuritySeedSize = 4;
inline constexpr std::size_t kSecurityKeySize = 4;

struct SecurityAccessRequest {
    std::uint8_t subFunction = static_cast<std::uint8_t>(SecuritySubFunction::RequestSeed);
    std::uint32_t key = 0; // only used for SendKey
};

struct SecurityAccessResponse {
    std::uint8_t subFunction = static_cast<std::uint8_t>(SecuritySubFunction::RequestSeed);
    std::uint32_t seed = 0; // only present for RequestSeed
};

[[nodiscard]] Result<SecurityAccessRequest> decodeSecurityAccessRequest(ConstBytes pdu) noexcept;
[[nodiscard]] ByteBuffer encodeSecurityAccessRequest(const SecurityAccessRequest &value) noexcept;
[[nodiscard]] Result<SecurityAccessResponse> decodeSecurityAccessResponse(ConstBytes pdu) noexcept;
[[nodiscard]] ByteBuffer encodeSecurityAccessResponse(const SecurityAccessResponse &value) noexcept;

// ---------------------------------------------------------------------------
// 0x22 / 0x2E Read and Write Data By Identifier (specification §26, §27)
// ---------------------------------------------------------------------------
struct ReadDidRequest {
    std::uint16_t did = 0;
};

struct ReadDidResponse {
    std::uint16_t did = 0;
    ByteBuffer data;

    [[nodiscard]] ConstBytes dataBytes() const noexcept { return bytesOf(data); }
};

struct WriteDidRequest {
    std::uint16_t did = 0;
    ByteBuffer data;

    [[nodiscard]] ConstBytes dataBytes() const noexcept { return bytesOf(data); }
};

struct WriteDidResponse {
    std::uint16_t did = 0;
};

[[nodiscard]] Result<ReadDidRequest> decodeReadDidRequest(ConstBytes pdu) noexcept;
[[nodiscard]] ByteBuffer encodeReadDidRequest(const ReadDidRequest &value) noexcept;
[[nodiscard]] Result<ReadDidResponse> decodeReadDidResponse(ConstBytes pdu) noexcept;
[[nodiscard]] ByteBuffer encodeReadDidResponse(const ReadDidResponse &value) noexcept;

[[nodiscard]] Result<WriteDidRequest> decodeWriteDidRequest(ConstBytes pdu) noexcept;
[[nodiscard]] ByteBuffer encodeWriteDidRequest(const WriteDidRequest &value) noexcept;
[[nodiscard]] Result<WriteDidResponse> decodeWriteDidResponse(ConstBytes pdu) noexcept;
[[nodiscard]] ByteBuffer encodeWriteDidResponse(const WriteDidResponse &value) noexcept;

// ---------------------------------------------------------------------------
// 0x31 Routine Control (specification §28)
// ---------------------------------------------------------------------------
struct RoutineControlRequest {
    std::uint8_t controlType = static_cast<std::uint8_t>(RoutineControlType::Start);
    std::uint16_t routineId = 0;
    ByteBuffer optionRecord;
};

struct RoutineControlResponse {
    std::uint8_t controlType = static_cast<std::uint8_t>(RoutineControlType::Start);
    std::uint16_t routineId = 0;
    std::uint8_t routineState = static_cast<std::uint8_t>(RoutineState::Idle);
    ByteBuffer statusRecord;
};

[[nodiscard]] Result<RoutineControlRequest> decodeRoutineControlRequest(ConstBytes pdu) noexcept;
[[nodiscard]] ByteBuffer encodeRoutineControlRequest(const RoutineControlRequest &value) noexcept;
[[nodiscard]] Result<RoutineControlResponse> decodeRoutineControlResponse(ConstBytes pdu) noexcept;
[[nodiscard]] ByteBuffer encodeRoutineControlResponse(const RoutineControlResponse &value) noexcept;

// ---------------------------------------------------------------------------
// 0x3E Client Present (specification §29)
// ---------------------------------------------------------------------------
struct ClientPresentRequest {
    std::uint8_t subFunction = 0x00;
};

struct ClientPresentResponse {
    std::uint8_t subFunction = 0x00;
};

[[nodiscard]] Result<ClientPresentRequest> decodeClientPresentRequest(ConstBytes pdu) noexcept;
[[nodiscard]] ByteBuffer encodeClientPresentRequest(const ClientPresentRequest &value = {}) noexcept;
[[nodiscard]] Result<ClientPresentResponse> decodeClientPresentResponse(ConstBytes pdu) noexcept;
[[nodiscard]] ByteBuffer encodeClientPresentResponse(const ClientPresentResponse &value = {}) noexcept;

// ---------------------------------------------------------------------------
// 0x40 Execute AT Command (specification §30)
// ---------------------------------------------------------------------------
struct ExecuteAtRequest {
    ByteBuffer command; // ASCII without CR/LF, 1..512 bytes
};

struct ExecuteAtResponse {
    ByteBuffer rawResponse;
};

[[nodiscard]] Result<ExecuteAtRequest> decodeExecuteAtRequest(ConstBytes pdu) noexcept;
[[nodiscard]] ByteBuffer encodeExecuteAtRequest(const ExecuteAtRequest &value) noexcept;
[[nodiscard]] Result<ExecuteAtResponse> decodeExecuteAtResponse(ConstBytes pdu) noexcept;
[[nodiscard]] ByteBuffer encodeExecuteAtResponse(const ExecuteAtResponse &value) noexcept;

// ---------------------------------------------------------------------------
// 0x41 Event Control (specification §31)
// ---------------------------------------------------------------------------
struct EventSubscribeRequest {
    std::uint16_t eventId = 0;
    std::uint8_t mode = 0;
    std::uint8_t flags = 0;
    std::uint16_t requestedPeriodMs = 0; // 0 == source-native rate
};

struct EventSubscribeResponse {
    std::uint16_t eventId = 0;
    std::uint16_t streamId = 0; // non-zero, scoped to one TCP connection
    std::uint8_t acceptedMode = 0;
    std::uint8_t flags = 0;
    std::uint16_t acceptedPeriodMs = 0;
    std::uint16_t queueCapacity = 0;
};

struct EventUnsubscribeRequest {
    std::uint16_t streamId = 0;
};

struct EventUnsubscribeResponse {
    std::uint16_t streamId = 0;
};

struct EventQueryRequest {
    std::uint16_t streamId = 0;
};

struct EventQueryResponse {
    std::uint16_t streamId = 0;
    std::uint16_t eventId = 0;
    std::uint8_t mode = 0;
    std::uint8_t state = 0;
    std::uint16_t periodMs = 0;
    std::uint32_t droppedCount = 0;
};

[[nodiscard]] Result<EventSubscribeRequest> decodeEventSubscribeRequest(ConstBytes pdu) noexcept;
[[nodiscard]] ByteBuffer encodeEventSubscribeRequest(const EventSubscribeRequest &value) noexcept;
[[nodiscard]] Result<EventSubscribeResponse> decodeEventSubscribeResponse(ConstBytes pdu) noexcept;
[[nodiscard]] ByteBuffer encodeEventSubscribeResponse(const EventSubscribeResponse &value) noexcept;

[[nodiscard]] Result<EventUnsubscribeRequest> decodeEventUnsubscribeRequest(ConstBytes pdu) noexcept;
[[nodiscard]] ByteBuffer encodeEventUnsubscribeRequest(const EventUnsubscribeRequest &value) noexcept;
[[nodiscard]] Result<EventUnsubscribeResponse> decodeEventUnsubscribeResponse(ConstBytes pdu) noexcept;
[[nodiscard]] ByteBuffer encodeEventUnsubscribeResponse(const EventUnsubscribeResponse &value) noexcept;

[[nodiscard]] Result<EventQueryRequest> decodeEventQueryRequest(ConstBytes pdu) noexcept;
[[nodiscard]] ByteBuffer encodeEventQueryRequest(const EventQueryRequest &value) noexcept;
[[nodiscard]] Result<EventQueryResponse> decodeEventQueryResponse(ConstBytes pdu) noexcept;
[[nodiscard]] ByteBuffer encodeEventQueryResponse(const EventQueryResponse &value) noexcept;

[[nodiscard]] ByteBuffer encodeEventUnsubscribeAllRequest() noexcept;
[[nodiscard]] Result<bool> decodeEventUnsubscribeAllRequest(ConstBytes pdu) noexcept;
[[nodiscard]] ByteBuffer encodeEventUnsubscribeAllResponse() noexcept;
[[nodiscard]] Result<bool> decodeEventUnsubscribeAllResponse(ConstBytes pdu) noexcept;

} // namespace uwb::protocol
