#include "uwb/protocol/services.hpp"

#include <utility>

#include "uwb/protocol/events.hpp"
#include "uwb/protocol/wire_reader.hpp"
#include "uwb/protocol/wire_writer.hpp"

namespace uwb::protocol {

namespace {

[[nodiscard]] bool isValidServiceNrc(std::uint8_t value) noexcept {
    switch (value) {
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x21: case 0x22: case 0x24: case 0x31:
    case 0x33: case 0x35: case 0x36: case 0x37: case 0x72: case 0x78: case 0x7E: case 0x7F:
        return true;
    default:
        return false;
    }
}

// Common helper: expect the PDU to start with the given SID and have exactly
// `exactSize` bytes (or at least `minSize` when exactSize == 0).
[[nodiscard]] ProtocolError checkPdu(ConstBytes pdu, std::uint8_t expectedSid, std::size_t exactSize,
                                    std::size_t minSize) noexcept {
    if (pdu.empty()) {
        return ProtocolError{ProtocolErrorCode::EmptyServicePdu, 0};
    }
    if (pdu[0] != expectedSid) {
        return ProtocolError{ProtocolErrorCode::UnknownServiceId, pdu[0]};
    }
    if (exactSize != 0) {
        if (pdu.size() != exactSize) {
            return ProtocolError{ProtocolErrorCode::UnexpectedPayloadLength, static_cast<std::uint32_t>(pdu.size())};
        }
        return {};
    }
    if (pdu.size() < minSize) {
        return ProtocolError{ProtocolErrorCode::UnexpectedPayloadLength, static_cast<std::uint32_t>(pdu.size())};
    }
    return {};
}

} // namespace

// ---------------------------------------------------------------------------
// Generic service PDU
// ---------------------------------------------------------------------------
Result<ServicePdu> decodeServicePdu(ConstBytes pdu) noexcept {
    if (pdu.empty()) {
        return Result<ServicePdu>::error(ProtocolErrorCode::EmptyServicePdu);
    }

    ServicePdu result;
    result.sid = pdu[0];
    result.body.assign(pdu.begin() + 1, pdu.end());

    if (result.negative()) {
        if (result.body.size() != 2) {
            return Result<ServicePdu>::error(ProtocolErrorCode::UnexpectedPayloadLength,
                                             static_cast<std::uint32_t>(result.body.size()));
        }
        if (!isValidServiceNrc(result.body[1])) {
            return Result<ServicePdu>::error(ProtocolErrorCode::InvalidField, result.body[1]);
        }
        return Result<ServicePdu>::ok(std::move(result));
    }

    if (!isKnownServiceId(result.sid)) {
        return Result<ServicePdu>::error(ProtocolErrorCode::UnknownServiceId, result.sid);
    }
    return Result<ServicePdu>::ok(std::move(result));
}

ByteBuffer encodeServicePdu(const ServicePdu &pdu) noexcept {
    ByteBuffer out;
    out.reserve(1 + pdu.body.size());
    out.push_back(pdu.sid);
    out.insert(out.end(), pdu.body.begin(), pdu.body.end());
    return out;
}

ByteBuffer encodeNegativeResponse(std::uint8_t requestSid, ServiceNrc nrc) noexcept {
    ByteBuffer out;
    out.reserve(3);
    WireWriter w{out};
    w.writeU8(kNegativeResponseSid);
    w.writeU8(requestSid);
    w.writeU8(static_cast<std::uint8_t>(nrc));
    return out;
}

Result<std::pair<std::uint8_t, ServiceNrc>> decodeNegativeResponse(ConstBytes pdu) noexcept {
    if (pdu.size() != 3) {
        return Result<std::pair<std::uint8_t, ServiceNrc>>::error(ProtocolErrorCode::UnexpectedPayloadLength,
                                                                  static_cast<std::uint32_t>(pdu.size()));
    }
    if (pdu[0] != kNegativeResponseSid) {
        return Result<std::pair<std::uint8_t, ServiceNrc>>::error(ProtocolErrorCode::UnknownServiceId, pdu[0]);
    }
    if (!isValidServiceNrc(pdu[2])) {
        return Result<std::pair<std::uint8_t, ServiceNrc>>::error(ProtocolErrorCode::InvalidField, pdu[2]);
    }

    return Result<std::pair<std::uint8_t, ServiceNrc>>::ok(
        std::make_pair(pdu[1], static_cast<ServiceNrc>(pdu[2])));
}

bool isPositiveResponseFor(std::uint8_t requestSid, ConstBytes pdu) noexcept {
    return !pdu.empty() && pdu[0] == positiveResponseSid(requestSid);
}

// ---------------------------------------------------------------------------
// 0x10 Session Control
// ---------------------------------------------------------------------------
Result<SessionControlRequest> decodeSessionControlRequest(ConstBytes pdu) noexcept {
    if (auto err = checkPdu(pdu, 0x10, 2, 0); err) {
        return Result<SessionControlRequest>::error(err);
    }
    if (!isKnownSessionId(pdu[1])) {
        return Result<SessionControlRequest>::error(ProtocolErrorCode::InvalidField, pdu[1]);
    }
    SessionControlRequest value;
    value.requestedSession = pdu[1];
    return Result<SessionControlRequest>::ok(value);
}

ByteBuffer encodeSessionControlRequest(const SessionControlRequest &value) noexcept {
    ByteBuffer out;
    out.reserve(2);
    WireWriter w{out};
    w.writeU8(0x10);
    w.writeU8(value.requestedSession);
    return out;
}

Result<SessionControlResponse> decodeSessionControlResponse(ConstBytes pdu) noexcept {
    if (auto err = checkPdu(pdu, 0x50, 6, 0); err) {
        return Result<SessionControlResponse>::error(err);
    }
    if (!isKnownSessionId(pdu[1])) {
        return Result<SessionControlResponse>::error(ProtocolErrorCode::InvalidField, pdu[1]);
    }

    WireReader reader{pdu.subspan(1)}; // skip SID
    SessionControlResponse value;
    if (!reader.readU8(value.activeSession) || !reader.readU16(value.p2ServerMaxMs) ||
        !reader.readU16(value.p2StarServerMax10ms)) {
        return Result<SessionControlResponse>::error(ProtocolErrorCode::TruncatedPayload);
    }
    return Result<SessionControlResponse>::ok(value);
}

ByteBuffer encodeSessionControlResponse(const SessionControlResponse &value) noexcept {
    ByteBuffer out;
    out.reserve(6);
    WireWriter w{out};
    w.writeU8(0x50);
    w.writeU8(value.activeSession);
    w.writeU16(value.p2ServerMaxMs);
    w.writeU16(value.p2StarServerMax10ms);
    return out;
}

// ---------------------------------------------------------------------------
// 0x11 Pico Device Reset
// ---------------------------------------------------------------------------
Result<DeviceResetRequest> decodeDeviceResetRequest(ConstBytes pdu) noexcept {
    if (auto err = checkPdu(pdu, 0x11, 2, 0); err) {
        return Result<DeviceResetRequest>::error(err);
    }
    if (!isKnownDeviceResetType(pdu[1])) {
        return Result<DeviceResetRequest>::error(ProtocolErrorCode::InvalidField, pdu[1]);
    }
    DeviceResetRequest value;
    value.resetType = pdu[1];
    return Result<DeviceResetRequest>::ok(value);
}

ByteBuffer encodeDeviceResetRequest(const DeviceResetRequest &value) noexcept {
    ByteBuffer out;
    out.reserve(2);
    WireWriter w{out};
    w.writeU8(0x11);
    w.writeU8(value.resetType);
    return out;
}

Result<DeviceResetResponse> decodeDeviceResetResponse(ConstBytes pdu) noexcept {
    if (auto err = checkPdu(pdu, 0x51, 2, 0); err) {
        return Result<DeviceResetResponse>::error(err);
    }
    if (!isKnownDeviceResetType(pdu[1])) {
        return Result<DeviceResetResponse>::error(ProtocolErrorCode::InvalidField, pdu[1]);
    }
    DeviceResetResponse value;
    value.resetType = pdu[1];
    return Result<DeviceResetResponse>::ok(value);
}

ByteBuffer encodeDeviceResetResponse(const DeviceResetResponse &value) noexcept {
    ByteBuffer out;
    out.reserve(2);
    WireWriter w{out};
    w.writeU8(0x51);
    w.writeU8(value.resetType);
    return out;
}

// ---------------------------------------------------------------------------
// 0x27 Security Access
// ---------------------------------------------------------------------------
Result<SecurityAccessRequest> decodeSecurityAccessRequest(ConstBytes pdu) noexcept {
    if (pdu.empty() || pdu[0] != 0x27) {
        return Result<SecurityAccessRequest>::error(
            pdu.empty() ? makeError(ProtocolErrorCode::EmptyServicePdu)
                        : makeError(ProtocolErrorCode::UnknownServiceId, pdu[0]));
    }
    if (pdu.size() != 2 && pdu.size() != 6) {
        return Result<SecurityAccessRequest>::error(ProtocolErrorCode::UnexpectedPayloadLength,
                                                    static_cast<std::uint32_t>(pdu.size()));
    }
    if (!isKnownSecuritySubFunction(pdu[1])) {
        return Result<SecurityAccessRequest>::error(ProtocolErrorCode::InvalidField, pdu[1]);
    }

    SecurityAccessRequest value;
    value.subFunction = pdu[1];

    if (value.subFunction == static_cast<std::uint8_t>(SecuritySubFunction::RequestSeed)) {
        if (pdu.size() != 2) {
            return Result<SecurityAccessRequest>::error(ProtocolErrorCode::UnexpectedPayloadLength,
                                                        static_cast<std::uint32_t>(pdu.size()));
        }
        return Result<SecurityAccessRequest>::ok(value);
    }

    WireReader reader{pdu.subspan(2)};
    if (!reader.readU32(value.key)) {
        return Result<SecurityAccessRequest>::error(ProtocolErrorCode::TruncatedPayload);
    }
    return Result<SecurityAccessRequest>::ok(value);
}

ByteBuffer encodeSecurityAccessRequest(const SecurityAccessRequest &value) noexcept {
    ByteBuffer out;
    out.reserve(6);
    WireWriter w{out};
    w.writeU8(0x27);
    w.writeU8(value.subFunction);
    if (value.subFunction == static_cast<std::uint8_t>(SecuritySubFunction::SendKey)) {
        w.writeU32(value.key);
    }
    return out;
}

Result<SecurityAccessResponse> decodeSecurityAccessResponse(ConstBytes pdu) noexcept {
    if (pdu.empty() || pdu[0] != 0x67) {
        return Result<SecurityAccessResponse>::error(
            pdu.empty() ? makeError(ProtocolErrorCode::EmptyServicePdu)
                        : makeError(ProtocolErrorCode::UnknownServiceId, pdu[0]));
    }
    if (pdu.size() != 2 && pdu.size() != 6) {
        return Result<SecurityAccessResponse>::error(ProtocolErrorCode::UnexpectedPayloadLength,
                                                     static_cast<std::uint32_t>(pdu.size()));
    }
    if (!isKnownSecuritySubFunction(pdu[1])) {
        return Result<SecurityAccessResponse>::error(ProtocolErrorCode::InvalidField, pdu[1]);
    }

    SecurityAccessResponse value;
    value.subFunction = pdu[1];

    if (value.subFunction == static_cast<std::uint8_t>(SecuritySubFunction::RequestSeed)) {
        WireReader reader{pdu.subspan(2)};
        if (!reader.readU32(value.seed)) {
            return Result<SecurityAccessResponse>::error(ProtocolErrorCode::TruncatedPayload);
        }
        return Result<SecurityAccessResponse>::ok(value);
    }

    if (pdu.size() != 2) {
        return Result<SecurityAccessResponse>::error(ProtocolErrorCode::UnexpectedPayloadLength,
                                                     static_cast<std::uint32_t>(pdu.size()));
    }
    return Result<SecurityAccessResponse>::ok(value);
}

ByteBuffer encodeSecurityAccessResponse(const SecurityAccessResponse &value) noexcept {
    ByteBuffer out;
    out.reserve(6);
    WireWriter w{out};
    w.writeU8(0x67);
    w.writeU8(value.subFunction);
    if (value.subFunction == static_cast<std::uint8_t>(SecuritySubFunction::RequestSeed)) {
        w.writeU32(value.seed);
    }
    return out;
}

// ---------------------------------------------------------------------------
// 0x22 / 0x2E Read and Write Data By Identifier
// ---------------------------------------------------------------------------
Result<ReadDidRequest> decodeReadDidRequest(ConstBytes pdu) noexcept {
    if (auto err = checkPdu(pdu, 0x22, 3, 0); err) {
        return Result<ReadDidRequest>::error(err);
    }
    WireReader reader{pdu.subspan(1)};
    ReadDidRequest value;
    if (!reader.readU16(value.did)) {
        return Result<ReadDidRequest>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (!isKnownDid(value.did)) {
        return Result<ReadDidRequest>::error(ProtocolErrorCode::InvalidField, value.did);
    }
    return Result<ReadDidRequest>::ok(value);
}

ByteBuffer encodeReadDidRequest(const ReadDidRequest &value) noexcept {
    ByteBuffer out;
    out.reserve(3);
    WireWriter w{out};
    w.writeU8(0x22);
    w.writeU16(value.did);
    return out;
}

Result<ReadDidResponse> decodeReadDidResponse(ConstBytes pdu) noexcept {
    if (auto err = checkPdu(pdu, 0x62, 0, 3); err) {
        return Result<ReadDidResponse>::error(err);
    }
    WireReader reader{pdu.subspan(1)};
    ReadDidResponse value;
    if (!reader.readU16(value.did)) {
        return Result<ReadDidResponse>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (!isKnownDid(value.did)) {
        return Result<ReadDidResponse>::error(ProtocolErrorCode::InvalidField, value.did);
    }
    value.data.assign(reader.remainingBytes().begin(), reader.remainingBytes().end());
    return Result<ReadDidResponse>::ok(std::move(value));
}

ByteBuffer encodeReadDidResponse(const ReadDidResponse &value) noexcept {
    ByteBuffer out;
    out.reserve(3 + value.data.size());
    WireWriter w{out};
    w.writeU8(0x62);
    w.writeU16(value.did);
    w.writeBytes(bytesOf(value.data));
    return out;
}

Result<WriteDidRequest> decodeWriteDidRequest(ConstBytes pdu) noexcept {
    if (auto err = checkPdu(pdu, 0x2E, 0, 3); err) {
        return Result<WriteDidRequest>::error(err);
    }
    WireReader reader{pdu.subspan(1)};
    WriteDidRequest value;
    if (!reader.readU16(value.did)) {
        return Result<WriteDidRequest>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (!isKnownDid(value.did)) {
        return Result<WriteDidRequest>::error(ProtocolErrorCode::InvalidField, value.did);
    }
    value.data.assign(reader.remainingBytes().begin(), reader.remainingBytes().end());
    return Result<WriteDidRequest>::ok(std::move(value));
}

ByteBuffer encodeWriteDidRequest(const WriteDidRequest &value) noexcept {
    ByteBuffer out;
    out.reserve(3 + value.data.size());
    WireWriter w{out};
    w.writeU8(0x2E);
    w.writeU16(value.did);
    w.writeBytes(bytesOf(value.data));
    return out;
}

Result<WriteDidResponse> decodeWriteDidResponse(ConstBytes pdu) noexcept {
    if (auto err = checkPdu(pdu, 0x6E, 3, 0); err) {
        return Result<WriteDidResponse>::error(err);
    }
    WireReader reader{pdu.subspan(1)};
    WriteDidResponse value;
    if (!reader.readU16(value.did)) {
        return Result<WriteDidResponse>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (!isKnownDid(value.did)) {
        return Result<WriteDidResponse>::error(ProtocolErrorCode::InvalidField, value.did);
    }
    return Result<WriteDidResponse>::ok(value);
}

ByteBuffer encodeWriteDidResponse(const WriteDidResponse &value) noexcept {
    ByteBuffer out;
    out.reserve(3);
    WireWriter w{out};
    w.writeU8(0x6E);
    w.writeU16(value.did);
    return out;
}

// ---------------------------------------------------------------------------
// 0x31 Routine Control
// ---------------------------------------------------------------------------
Result<RoutineControlRequest> decodeRoutineControlRequest(ConstBytes pdu) noexcept {
    if (auto err = checkPdu(pdu, 0x31, 0, 4); err) {
        return Result<RoutineControlRequest>::error(err);
    }
    if (!isKnownRoutineControlType(pdu[1])) {
        return Result<RoutineControlRequest>::error(ProtocolErrorCode::InvalidField, pdu[1]);
    }

    WireReader reader{pdu.subspan(1)};
    RoutineControlRequest value;
    if (!reader.readU8(value.controlType) || !reader.readU16(value.routineId)) {
        return Result<RoutineControlRequest>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (!isKnownRoutine(value.routineId)) {
        return Result<RoutineControlRequest>::error(ProtocolErrorCode::InvalidField, value.routineId);
    }
    value.optionRecord.assign(reader.remainingBytes().begin(), reader.remainingBytes().end());
    return Result<RoutineControlRequest>::ok(std::move(value));
}

ByteBuffer encodeRoutineControlRequest(const RoutineControlRequest &value) noexcept {
    ByteBuffer out;
    out.reserve(4 + value.optionRecord.size());
    WireWriter w{out};
    w.writeU8(0x31);
    w.writeU8(value.controlType);
    w.writeU16(value.routineId);
    w.writeBytes(bytesOf(value.optionRecord));
    return out;
}

Result<RoutineControlResponse> decodeRoutineControlResponse(ConstBytes pdu) noexcept {
    if (auto err = checkPdu(pdu, 0x71, 0, 5); err) {
        return Result<RoutineControlResponse>::error(err);
    }
    if (!isKnownRoutineControlType(pdu[1])) {
        return Result<RoutineControlResponse>::error(ProtocolErrorCode::InvalidField, pdu[1]);
    }

    WireReader reader{pdu.subspan(1)};
    RoutineControlResponse value;
    if (!reader.readU8(value.controlType) || !reader.readU16(value.routineId)) {
        return Result<RoutineControlResponse>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (!reader.readU8(value.routineState) || !isKnownRoutineState(value.routineState)) {
        return Result<RoutineControlResponse>::error(ProtocolErrorCode::InvalidField, value.routineState);
    }
    if (!isKnownRoutine(value.routineId)) {
        return Result<RoutineControlResponse>::error(ProtocolErrorCode::InvalidField, value.routineId);
    }
    value.statusRecord.assign(reader.remainingBytes().begin(), reader.remainingBytes().end());
    return Result<RoutineControlResponse>::ok(std::move(value));
}

ByteBuffer encodeRoutineControlResponse(const RoutineControlResponse &value) noexcept {
    ByteBuffer out;
    out.reserve(5 + value.statusRecord.size());
    WireWriter w{out};
    w.writeU8(0x71);
    w.writeU8(value.controlType);
    w.writeU16(value.routineId);
    w.writeU8(value.routineState);
    w.writeBytes(bytesOf(value.statusRecord));
    return out;
}

// ---------------------------------------------------------------------------
// 0x3E Client Present
// ---------------------------------------------------------------------------
Result<ClientPresentRequest> decodeClientPresentRequest(ConstBytes pdu) noexcept {
    if (auto err = checkPdu(pdu, 0x3E, 2, 0); err) {
        return Result<ClientPresentRequest>::error(err);
    }
    if (pdu[1] != 0x00) {
        return Result<ClientPresentRequest>::error(ProtocolErrorCode::InvalidField, pdu[1]);
    }
    ClientPresentRequest value;
    value.subFunction = pdu[1];
    return Result<ClientPresentRequest>::ok(value);
}

ByteBuffer encodeClientPresentRequest(const ClientPresentRequest &value) noexcept {
    ByteBuffer out;
    out.reserve(2);
    WireWriter w{out};
    w.writeU8(0x3E);
    w.writeU8(value.subFunction);
    return out;
}

Result<ClientPresentResponse> decodeClientPresentResponse(ConstBytes pdu) noexcept {
    if (auto err = checkPdu(pdu, 0x7E, 2, 0); err) {
        return Result<ClientPresentResponse>::error(err);
    }
    if (pdu[1] != 0x00) {
        return Result<ClientPresentResponse>::error(ProtocolErrorCode::InvalidField, pdu[1]);
    }
    ClientPresentResponse value;
    value.subFunction = pdu[1];
    return Result<ClientPresentResponse>::ok(value);
}

ByteBuffer encodeClientPresentResponse(const ClientPresentResponse &value) noexcept {
    ByteBuffer out;
    out.reserve(2);
    WireWriter w{out};
    w.writeU8(0x7E);
    w.writeU8(value.subFunction);
    return out;
}

// ---------------------------------------------------------------------------
// 0x40 Execute AT Command
// ---------------------------------------------------------------------------
namespace {

[[nodiscard]] bool containsCrLf(ConstBytes bytes) noexcept {
    for (std::uint8_t b : bytes) {
        if (b == '\r' || b == '\n') {
            return true;
        }
    }
    return false;
}

} // namespace

Result<ExecuteAtRequest> decodeExecuteAtRequest(ConstBytes pdu) noexcept {
    if (auto err = checkPdu(pdu, 0x40, 0, 3); err) {
        return Result<ExecuteAtRequest>::error(err);
    }

    WireReader reader{pdu.subspan(1)};

    std::uint16_t commandLength = 0;
    if (!reader.readU16(commandLength)) {
        return Result<ExecuteAtRequest>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (commandLength == 0 || commandLength > kMaxAtCommandLength) {
        return Result<ExecuteAtRequest>::error(ProtocolErrorCode::InvalidField, commandLength);
    }
    if (reader.remaining() != commandLength) {
        return Result<ExecuteAtRequest>::error(ProtocolErrorCode::UnexpectedPayloadLength,
                                               static_cast<std::uint32_t>(pdu.size()));
    }

    ConstBytes command = reader.remainingBytes();
    if (containsCrLf(command)) {
        // Specification §30: the command SHALL NOT include CR/LF.
        return Result<ExecuteAtRequest>::error(ProtocolErrorCode::InvalidField);
    }

    ExecuteAtRequest value;
    value.command.assign(command.begin(), command.end());
    return Result<ExecuteAtRequest>::ok(std::move(value));
}

ByteBuffer encodeExecuteAtRequest(const ExecuteAtRequest &value) noexcept {
    ByteBuffer out;
    out.reserve(3 + value.command.size());
    WireWriter w{out};
    w.writeU8(0x40);
    w.writeU16(static_cast<std::uint16_t>(value.command.size()));
    w.writeBytes(bytesOf(value.command));
    return out;
}

Result<ExecuteAtResponse> decodeExecuteAtResponse(ConstBytes pdu) noexcept {
    if (auto err = checkPdu(pdu, 0x80, 0, 2); err) {
        return Result<ExecuteAtResponse>::error(err);
    }

    WireReader reader{pdu.subspan(1)};

    std::uint16_t responseLength = 0;
    if (!reader.readU16(responseLength)) {
        return Result<ExecuteAtResponse>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (reader.remaining() != responseLength) {
        return Result<ExecuteAtResponse>::error(ProtocolErrorCode::UnexpectedPayloadLength,
                                                static_cast<std::uint32_t>(pdu.size()));
    }

    ExecuteAtResponse value;
    ConstBytes raw = reader.remainingBytes();
    value.rawResponse.assign(raw.begin(), raw.end());
    return Result<ExecuteAtResponse>::ok(std::move(value));
}

ByteBuffer encodeExecuteAtResponse(const ExecuteAtResponse &value) noexcept {
    ByteBuffer out;
    out.reserve(2 + value.rawResponse.size());
    WireWriter w{out};
    w.writeU8(0x80);
    w.writeU16(static_cast<std::uint16_t>(value.rawResponse.size()));
    w.writeBytes(bytesOf(value.rawResponse));
    return out;
}

// ---------------------------------------------------------------------------
// 0x41 Event Control
// ---------------------------------------------------------------------------
Result<EventSubscribeRequest> decodeEventSubscribeRequest(ConstBytes pdu) noexcept {
    if (auto err = checkPdu(pdu, 0x41, 8, 0); err) {
        return Result<EventSubscribeRequest>::error(err);
    }
    if (pdu[1] != static_cast<std::uint8_t>(EventControlSubFunction::Subscribe)) {
        return Result<EventSubscribeRequest>::error(ProtocolErrorCode::InvalidField, pdu[1]);
    }

    WireReader reader{pdu.subspan(2)};
    EventSubscribeRequest value;
    if (!reader.readU16(value.eventId)) {
        return Result<EventSubscribeRequest>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (!reader.readU8(value.mode) || !reader.readU8(value.flags) || !reader.readU16(value.requestedPeriodMs)) {
        return Result<EventSubscribeRequest>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (!isKnownEventId(value.eventId)) {
        return Result<EventSubscribeRequest>::error(ProtocolErrorCode::InvalidField, value.eventId);
    }
    if (!isKnownStreamMode(value.mode)) {
        return Result<EventSubscribeRequest>::error(ProtocolErrorCode::InvalidField, value.mode);
    }
    return Result<EventSubscribeRequest>::ok(value);
}

ByteBuffer encodeEventSubscribeRequest(const EventSubscribeRequest &value) noexcept {
    ByteBuffer out;
    out.reserve(8);
    WireWriter w{out};
    w.writeU8(0x41);
    w.writeU8(static_cast<std::uint8_t>(EventControlSubFunction::Subscribe));
    w.writeU16(value.eventId);
    w.writeU8(value.mode);
    w.writeU8(value.flags);
    w.writeU16(value.requestedPeriodMs);
    return out;
}

Result<EventSubscribeResponse> decodeEventSubscribeResponse(ConstBytes pdu) noexcept {
    if (auto err = checkPdu(pdu, 0x81, 12, 0); err) {
        return Result<EventSubscribeResponse>::error(err);
    }
    if (pdu[1] != static_cast<std::uint8_t>(EventControlSubFunction::Subscribe)) {
        return Result<EventSubscribeResponse>::error(ProtocolErrorCode::InvalidField, pdu[1]);
    }

    WireReader reader{pdu.subspan(2)};
    EventSubscribeResponse value;
    if (!reader.readU16(value.eventId) || !reader.readU16(value.streamId)) {
        return Result<EventSubscribeResponse>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (!reader.readU8(value.acceptedMode) || !reader.readU8(value.flags)) {
        return Result<EventSubscribeResponse>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (!reader.readU16(value.acceptedPeriodMs) || !reader.readU16(value.queueCapacity)) {
        return Result<EventSubscribeResponse>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (!isKnownEventId(value.eventId)) {
        return Result<EventSubscribeResponse>::error(ProtocolErrorCode::InvalidField, value.eventId);
    }
    if (!isKnownStreamMode(value.acceptedMode)) {
        return Result<EventSubscribeResponse>::error(ProtocolErrorCode::InvalidField, value.acceptedMode);
    }
    if (value.streamId == 0) {
        // Specification §31.4: successful subscriptions always return a non-zero streamId.
        return Result<EventSubscribeResponse>::error(ProtocolErrorCode::InvalidField, value.streamId);
    }
    return Result<EventSubscribeResponse>::ok(value);
}

ByteBuffer encodeEventSubscribeResponse(const EventSubscribeResponse &value) noexcept {
    ByteBuffer out;
    out.reserve(12);
    WireWriter w{out};
    w.writeU8(0x81);
    w.writeU8(static_cast<std::uint8_t>(EventControlSubFunction::Subscribe));
    w.writeU16(value.eventId);
    w.writeU16(value.streamId);
    w.writeU8(value.acceptedMode);
    w.writeU8(value.flags);
    w.writeU16(value.acceptedPeriodMs);
    w.writeU16(value.queueCapacity);
    return out;
}

Result<EventUnsubscribeRequest> decodeEventUnsubscribeRequest(ConstBytes pdu) noexcept {
    if (auto err = checkPdu(pdu, 0x41, 4, 0); err) {
        return Result<EventUnsubscribeRequest>::error(err);
    }
    if (pdu[1] != static_cast<std::uint8_t>(EventControlSubFunction::Unsubscribe)) {
        return Result<EventUnsubscribeRequest>::error(ProtocolErrorCode::InvalidField, pdu[1]);
    }

    WireReader reader{pdu.subspan(2)};
    EventUnsubscribeRequest value;
    if (!reader.readU16(value.streamId)) {
        return Result<EventUnsubscribeRequest>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (value.streamId == 0) {
        return Result<EventUnsubscribeRequest>::error(ProtocolErrorCode::InvalidField, value.streamId);
    }
    return Result<EventUnsubscribeRequest>::ok(value);
}

ByteBuffer encodeEventUnsubscribeRequest(const EventUnsubscribeRequest &value) noexcept {
    ByteBuffer out;
    out.reserve(4);
    WireWriter w{out};
    w.writeU8(0x41);
    w.writeU8(static_cast<std::uint8_t>(EventControlSubFunction::Unsubscribe));
    w.writeU16(value.streamId);
    return out;
}

Result<EventUnsubscribeResponse> decodeEventUnsubscribeResponse(ConstBytes pdu) noexcept {
    if (auto err = checkPdu(pdu, 0x81, 4, 0); err) {
        return Result<EventUnsubscribeResponse>::error(err);
    }
    if (pdu[1] != static_cast<std::uint8_t>(EventControlSubFunction::Unsubscribe)) {
        return Result<EventUnsubscribeResponse>::error(ProtocolErrorCode::InvalidField, pdu[1]);
    }

    WireReader reader{pdu.subspan(2)};
    EventUnsubscribeResponse value;
    if (!reader.readU16(value.streamId)) {
        return Result<EventUnsubscribeResponse>::error(ProtocolErrorCode::TruncatedPayload);
    }
    return Result<EventUnsubscribeResponse>::ok(value);
}

ByteBuffer encodeEventUnsubscribeResponse(const EventUnsubscribeResponse &value) noexcept {
    ByteBuffer out;
    out.reserve(4);
    WireWriter w{out};
    w.writeU8(0x81);
    w.writeU8(static_cast<std::uint8_t>(EventControlSubFunction::Unsubscribe));
    w.writeU16(value.streamId);
    return out;
}

Result<EventQueryRequest> decodeEventQueryRequest(ConstBytes pdu) noexcept {
    if (auto err = checkPdu(pdu, 0x41, 4, 0); err) {
        return Result<EventQueryRequest>::error(err);
    }
    if (pdu[1] != static_cast<std::uint8_t>(EventControlSubFunction::QuerySubscription)) {
        return Result<EventQueryRequest>::error(ProtocolErrorCode::InvalidField, pdu[1]);
    }

    WireReader reader{pdu.subspan(2)};
    EventQueryRequest value;
    if (!reader.readU16(value.streamId)) {
        return Result<EventQueryRequest>::error(ProtocolErrorCode::TruncatedPayload);
    }
    return Result<EventQueryRequest>::ok(value);
}

ByteBuffer encodeEventQueryRequest(const EventQueryRequest &value) noexcept {
    ByteBuffer out;
    out.reserve(4);
    WireWriter w{out};
    w.writeU8(0x41);
    w.writeU8(static_cast<std::uint8_t>(EventControlSubFunction::QuerySubscription));
    w.writeU16(value.streamId);
    return out;
}

Result<EventQueryResponse> decodeEventQueryResponse(ConstBytes pdu) noexcept {
    if (auto err = checkPdu(pdu, 0x81, 14, 0); err) {
        return Result<EventQueryResponse>::error(err);
    }
    if (pdu[1] != static_cast<std::uint8_t>(EventControlSubFunction::QuerySubscription)) {
        return Result<EventQueryResponse>::error(ProtocolErrorCode::InvalidField, pdu[1]);
    }

    WireReader reader{pdu.subspan(2)};
    EventQueryResponse value;
    if (!reader.readU16(value.streamId) || !reader.readU16(value.eventId)) {
        return Result<EventQueryResponse>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (!reader.readU8(value.mode) || !reader.readU8(value.state)) {
        return Result<EventQueryResponse>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (!reader.readU16(value.periodMs) || !reader.readU32(value.droppedCount)) {
        return Result<EventQueryResponse>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (!isKnownStreamMode(value.mode)) {
        return Result<EventQueryResponse>::error(ProtocolErrorCode::InvalidField, value.mode);
    }
    if (!isKnownStreamState(value.state)) {
        return Result<EventQueryResponse>::error(ProtocolErrorCode::InvalidField, value.state);
    }
    return Result<EventQueryResponse>::ok(value);
}

ByteBuffer encodeEventQueryResponse(const EventQueryResponse &value) noexcept {
    ByteBuffer out;
    out.reserve(14);
    WireWriter w{out};
    w.writeU8(0x81);
    w.writeU8(static_cast<std::uint8_t>(EventControlSubFunction::QuerySubscription));
    w.writeU16(value.streamId);
    w.writeU16(value.eventId);
    w.writeU8(value.mode);
    w.writeU8(value.state);
    w.writeU16(value.periodMs);
    w.writeU32(value.droppedCount);
    return out;
}

ByteBuffer encodeEventUnsubscribeAllRequest() noexcept {
    ByteBuffer out;
    out.reserve(2);
    WireWriter w{out};
    w.writeU8(0x41);
    w.writeU8(static_cast<std::uint8_t>(EventControlSubFunction::UnsubscribeAll));
    return out;
}

Result<bool> decodeEventUnsubscribeAllRequest(ConstBytes pdu) noexcept {
    if (auto err = checkPdu(pdu, 0x41, 2, 0); err) {
        return Result<bool>::error(err);
    }
    if (pdu[1] != static_cast<std::uint8_t>(EventControlSubFunction::UnsubscribeAll)) {
        return Result<bool>::error(ProtocolErrorCode::InvalidField, pdu[1]);
    }
    return Result<bool>::ok(true);
}

ByteBuffer encodeEventUnsubscribeAllResponse() noexcept {
    ByteBuffer out;
    out.reserve(2);
    WireWriter w{out};
    w.writeU8(0x81);
    w.writeU8(static_cast<std::uint8_t>(EventControlSubFunction::UnsubscribeAll));
    return out;
}

Result<bool> decodeEventUnsubscribeAllResponse(ConstBytes pdu) noexcept {
    if (auto err = checkPdu(pdu, 0x81, 2, 0); err) {
        return Result<bool>::error(err);
    }
    if (pdu[1] != static_cast<std::uint8_t>(EventControlSubFunction::UnsubscribeAll)) {
        return Result<bool>::error(ProtocolErrorCode::InvalidField, pdu[1]);
    }
    return Result<bool>::ok(true);
}

} // namespace uwb::protocol
