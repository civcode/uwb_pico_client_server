#include "uwb/protocol/payloads.hpp"

#include "uwb/protocol/dids.hpp"
#include "uwb/protocol/wire_reader.hpp"
#include "uwb/protocol/wire_writer.hpp"

namespace uwb::protocol {

namespace {

constexpr std::size_t kDeviceIdResponseFixedSize = 16 + 2 + 2 + 8 + 1 + 1 + 1 + 1;
constexpr std::size_t kActivationRequestSize = 2 + 1 + 1 + 16;
constexpr std::size_t kActivationResponseSize = 2 + 1 + 1 + 16 + 4 + 8;
constexpr std::size_t kApplicationEnvelopeFixedSize = 2 + 2 + 4 + 1 + 3;

[[nodiscard]] bool isValidNackCode(std::uint8_t code) noexcept {
    return code <= static_cast<std::uint8_t>(GenericHeaderNackCode::InvalidInCurrentState);
}

[[nodiscard]] bool isValidActivationResponseCode(std::uint8_t code) noexcept {
    switch (code) {
    case 0x00: case 0x01: case 0x10: case 0x11: case 0x12: case 0x13:
        return true;
    default:
        return false;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Generic Header NACK
// ---------------------------------------------------------------------------
Result<GenericHeaderNack> decodeGenericHeaderNack(ConstBytes payload) noexcept {
    if (payload.size() != 1) {
        return Result<GenericHeaderNack>::error(ProtocolErrorCode::UnexpectedPayloadLength,
                                                static_cast<std::uint32_t>(payload.size()));
    }

    const std::uint8_t raw = payload[0];
    if (!isValidNackCode(raw)) {
        return Result<GenericHeaderNack>::error(ProtocolErrorCode::InvalidField, raw);
    }

    GenericHeaderNack value;
    value.code = static_cast<GenericHeaderNackCode>(raw);
    return Result<GenericHeaderNack>::ok(value);
}

ByteBuffer encodeGenericHeaderNack(const GenericHeaderNack &value) noexcept {
    ByteBuffer out;
    out.reserve(1);
    WireWriter w{out};
    w.writeU8(static_cast<std::uint8_t>(value.code));
    return out;
}

// ---------------------------------------------------------------------------
// Device Identification Request
// ---------------------------------------------------------------------------
Result<DeviceIdRequest> decodeDeviceIdRequest(ConstBytes payload) noexcept {
    // Specification §16: payload length SHALL be zero in v1; a non-zero payload is
    // rejected as invalid format.
    if (!payload.empty()) {
        return Result<DeviceIdRequest>::error(ProtocolErrorCode::UnexpectedPayloadLength,
                                              static_cast<std::uint32_t>(payload.size()));
    }
    return Result<DeviceIdRequest>::ok(DeviceIdRequest{});
}

ByteBuffer encodeDeviceIdRequest(const DeviceIdRequest &) noexcept {
    return ByteBuffer{};
}

// ---------------------------------------------------------------------------
// Device Identification Response
// ---------------------------------------------------------------------------
Result<DeviceIdResponse> decodeDeviceIdResponse(ConstBytes payload) noexcept {
    if (payload.size() < kDeviceIdResponseFixedSize) {
        return Result<DeviceIdResponse>::error(ProtocolErrorCode::TruncatedPayload,
                                               static_cast<std::uint32_t>(payload.size()));
    }

    WireReader reader{payload};
    DeviceIdResponse value;

    if (!reader.readArray(value.deviceUuid.bytes)) {
        return Result<DeviceIdResponse>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (!reader.readU16(value.logicalAddress) || !reader.readU16(value.tcpPort)) {
        return Result<DeviceIdResponse>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (!reader.readU64(value.capabilityMask)) {
        return Result<DeviceIdResponse>::error(ProtocolErrorCode::TruncatedPayload);
    }

    std::uint8_t controlStatus = 0;
    if (!reader.readU8(controlStatus) || (controlStatus != 0x00 && controlStatus != 0x01)) {
        return Result<DeviceIdResponse>::error(ProtocolErrorCode::InvalidField, controlStatus);
    }
    value.controlStatus = static_cast<ControlStatus>(controlStatus);

    if (!reader.readU8(value.activeObservers) || !reader.readU8(value.maxObservers)) {
        return Result<DeviceIdResponse>::error(ProtocolErrorCode::TruncatedPayload);
    }

    std::uint8_t nameLength = 0;
    if (!reader.readU8(nameLength)) {
        return Result<DeviceIdResponse>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (nameLength > kMaxDeviceNameLength) {
        return Result<DeviceIdResponse>::error(ProtocolErrorCode::InvalidField, nameLength);
    }
    if (reader.remaining() != nameLength) {
        return Result<DeviceIdResponse>::error(ProtocolErrorCode::UnexpectedPayloadLength,
                                               static_cast<std::uint32_t>(payload.size()));
    }

    const ConstBytes nameBytes = reader.remainingBytes();
    value.deviceName.assign(reinterpret_cast<const char *>(nameBytes.data()), nameLength);
    return Result<DeviceIdResponse>::ok(std::move(value));
}

ByteBuffer encodeDeviceIdResponse(const DeviceIdResponse &value) noexcept {
    ByteBuffer out;
    out.reserve(kDeviceIdResponseFixedSize + value.deviceName.size());
    WireWriter w{out};
    w.writeArray(value.deviceUuid.bytes);
    w.writeU16(value.logicalAddress);
    w.writeU16(value.tcpPort);
    w.writeU64(value.capabilityMask);
    w.writeU8(static_cast<std::uint8_t>(value.controlStatus));
    w.writeU8(value.activeObservers);
    w.writeU8(value.maxObservers);
    w.writeLengthPrefixedString8(value.deviceName);
    return out;
}

// ---------------------------------------------------------------------------
// Connection Activation
// ---------------------------------------------------------------------------
Result<ConnectionActivationRequest> decodeConnectionActivationRequest(ConstBytes payload) noexcept {
    if (payload.size() != kActivationRequestSize) {
        return Result<ConnectionActivationRequest>::error(ProtocolErrorCode::UnexpectedPayloadLength,
                                                          static_cast<std::uint32_t>(payload.size()));
    }

    WireReader reader{payload};
    ConnectionActivationRequest value;

    if (!reader.readU16(value.clientLogicalAddress)) {
        return Result<ConnectionActivationRequest>::error(ProtocolErrorCode::TruncatedPayload);
    }

    std::uint8_t role = 0;
    if (!reader.readU8(role) || !isKnownConnectionRole(role)) {
        return Result<ConnectionActivationRequest>::error(ProtocolErrorCode::InvalidField, role);
    }
    value.requestedRole = static_cast<ConnectionRole>(role);

    if (!reader.readU8(value.flags) || value.flags != 0) {
        return Result<ConnectionActivationRequest>::error(ProtocolErrorCode::InvalidField, value.flags);
    }
    if (!reader.readArray(value.clientInstanceUuid.bytes)) {
        return Result<ConnectionActivationRequest>::error(ProtocolErrorCode::TruncatedPayload);
    }

    return Result<ConnectionActivationRequest>::ok(value);
}

ByteBuffer encodeConnectionActivationRequest(const ConnectionActivationRequest &value) noexcept {
    ByteBuffer out;
    out.reserve(kActivationRequestSize);
    WireWriter w{out};
    w.writeU16(value.clientLogicalAddress);
    w.writeU8(static_cast<std::uint8_t>(value.requestedRole));
    w.writeU8(value.flags);
    w.writeArray(value.clientInstanceUuid.bytes);
    return out;
}

Result<ConnectionActivationResponse> decodeConnectionActivationResponse(ConstBytes payload) noexcept {
    if (payload.size() != kActivationResponseSize) {
        return Result<ConnectionActivationResponse>::error(ProtocolErrorCode::UnexpectedPayloadLength,
                                                           static_cast<std::uint32_t>(payload.size()));
    }

    WireReader reader{payload};
    ConnectionActivationResponse value;

    if (!reader.readU16(value.serverLogicalAddress)) {
        return Result<ConnectionActivationResponse>::error(ProtocolErrorCode::TruncatedPayload);
    }

    std::uint8_t role = 0;
    if (!reader.readU8(role) || (role != 0x00 && !isKnownConnectionRole(role))) {
        return Result<ConnectionActivationResponse>::error(ProtocolErrorCode::InvalidField, role);
    }
    value.assignedRole = static_cast<ConnectionRole>(role);

    std::uint8_t code = 0;
    if (!reader.readU8(code) || !isValidActivationResponseCode(code)) {
        return Result<ConnectionActivationResponse>::error(ProtocolErrorCode::InvalidField, code);
    }
    value.responseCode = static_cast<ActivationResponseCode>(code);

    if (!reader.readArray(value.deviceUuid.bytes)) {
        return Result<ConnectionActivationResponse>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (!reader.readU32(value.serverMaxPayload)) {
        return Result<ConnectionActivationResponse>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (value.serverMaxPayload > kMaxProtocolPayload) {
        return Result<ConnectionActivationResponse>::error(ProtocolErrorCode::InvalidField,
                                                            value.serverMaxPayload);
    }
    if (!reader.readU64(value.capabilityMask)) {
        return Result<ConnectionActivationResponse>::error(ProtocolErrorCode::TruncatedPayload);
    }

    return Result<ConnectionActivationResponse>::ok(value);
}

ByteBuffer encodeConnectionActivationResponse(const ConnectionActivationResponse &value) noexcept {
    ByteBuffer out;
    out.reserve(kActivationResponseSize);
    WireWriter w{out};
    w.writeU16(value.serverLogicalAddress);
    w.writeU8(static_cast<std::uint8_t>(value.assignedRole));
    w.writeU8(static_cast<std::uint8_t>(value.responseCode));
    w.writeArray(value.deviceUuid.bytes);
    w.writeU32(value.serverMaxPayload);
    w.writeU64(value.capabilityMask);
    return out;
}

// ---------------------------------------------------------------------------
// Alive Check
// ---------------------------------------------------------------------------
Result<AliveCheckRequest> decodeAliveCheckRequest(ConstBytes payload) noexcept {
    if (payload.size() != 8) {
        return Result<AliveCheckRequest>::error(ProtocolErrorCode::UnexpectedPayloadLength,
                                                static_cast<std::uint32_t>(payload.size()));
    }
    WireReader reader{payload};
    AliveCheckRequest value;
    if (!reader.readU64(value.nonce)) {
        return Result<AliveCheckRequest>::error(ProtocolErrorCode::TruncatedPayload);
    }
    return Result<AliveCheckRequest>::ok(value);
}

ByteBuffer encodeAliveCheckRequest(const AliveCheckRequest &value) noexcept {
    ByteBuffer out;
    out.reserve(8);
    WireWriter w{out};
    w.writeU64(value.nonce);
    return out;
}

Result<AliveCheckResponse> decodeAliveCheckResponse(ConstBytes payload) noexcept {
    if (payload.size() != 8) {
        return Result<AliveCheckResponse>::error(ProtocolErrorCode::UnexpectedPayloadLength,
                                                 static_cast<std::uint32_t>(payload.size()));
    }
    WireReader reader{payload};
    AliveCheckResponse value;
    if (!reader.readU64(value.nonce)) {
        return Result<AliveCheckResponse>::error(ProtocolErrorCode::TruncatedPayload);
    }
    return Result<AliveCheckResponse>::ok(value);
}

ByteBuffer encodeAliveCheckResponse(const AliveCheckResponse &value) noexcept {
    ByteBuffer out;
    out.reserve(8);
    WireWriter w{out};
    w.writeU64(value.nonce);
    return out;
}

// ---------------------------------------------------------------------------
// Application Message envelope
// ---------------------------------------------------------------------------
Result<ApplicationEnvelope> decodeApplicationEnvelope(ConstBytes payload) noexcept {
    if (payload.size() < kApplicationEnvelopeFixedSize) {
        return Result<ApplicationEnvelope>::error(ProtocolErrorCode::UnexpectedPayloadLength,
                                                  static_cast<std::uint32_t>(payload.size()));
    }

    WireReader reader{payload};
    ApplicationEnvelope value;

    if (!reader.readU16(value.sourceLogicalAddress) || !reader.readU16(value.targetLogicalAddress)) {
        return Result<ApplicationEnvelope>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (!reader.readU32(value.transactionId)) {
        return Result<ApplicationEnvelope>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (!reader.readU8(value.flags)) {
        return Result<ApplicationEnvelope>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if ((value.flags & kApplicationFlagReservedMask) != 0) {
        return Result<ApplicationEnvelope>::error(ProtocolErrorCode::InvalidReservedBits, value.flags);
    }

    std::uint8_t reserved0 = 0;
    std::uint8_t reserved1 = 0;
    std::uint8_t reserved2 = 0;
    if (!reader.readU8(reserved0) || !reader.readU8(reserved1) || !reader.readU8(reserved2)) {
        return Result<ApplicationEnvelope>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (reserved0 != 0 || reserved1 != 0 || reserved2 != 0) {
        return Result<ApplicationEnvelope>::error(ProtocolErrorCode::InvalidReservedBits, reserved0);
    }

    value.servicePdu.assign(reader.remainingBytes().begin(), reader.remainingBytes().end());
    return Result<ApplicationEnvelope>::ok(std::move(value));
}

ByteBuffer encodeApplicationEnvelope(const ApplicationEnvelope &value) noexcept {
    ByteBuffer out;
    out.reserve(kApplicationEnvelopeFixedSize + value.servicePdu.size());
    WireWriter w{out};
    w.writeU16(value.sourceLogicalAddress);
    w.writeU16(value.targetLogicalAddress);
    w.writeU32(value.transactionId);
    w.writeU8(value.flags);
    w.writeU8(0);
    w.writeU8(0);
    w.writeU8(0);
    w.writeBytes(bytesOf(value.servicePdu));
    return out;
}

// ---------------------------------------------------------------------------
// Application Message ACK / NACK
// ---------------------------------------------------------------------------
Result<ApplicationMessageAck> decodeApplicationMessageAck(ConstBytes payload) noexcept {
    if (payload.size() != 8) {
        return Result<ApplicationMessageAck>::error(ProtocolErrorCode::UnexpectedPayloadLength,
                                                    static_cast<std::uint32_t>(payload.size()));
    }
    WireReader reader{payload};
    ApplicationMessageAck value;
    if (!reader.readU16(value.sourceLogicalAddress) || !reader.readU16(value.targetLogicalAddress) ||
        !reader.readU32(value.transactionId)) {
        return Result<ApplicationMessageAck>::error(ProtocolErrorCode::TruncatedPayload);
    }
    return Result<ApplicationMessageAck>::ok(value);
}

ByteBuffer encodeApplicationMessageAck(const ApplicationMessageAck &value) noexcept {
    ByteBuffer out;
    out.reserve(8);
    WireWriter w{out};
    w.writeU16(value.sourceLogicalAddress);
    w.writeU16(value.targetLogicalAddress);
    w.writeU32(value.transactionId);
    return out;
}

Result<ApplicationMessageNack> decodeApplicationMessageNack(ConstBytes payload) noexcept {
    if (payload.size() != 9) {
        return Result<ApplicationMessageNack>::error(ProtocolErrorCode::UnexpectedPayloadLength,
                                                     static_cast<std::uint32_t>(payload.size()));
    }
    WireReader reader{payload};
    ApplicationMessageNack value;
    if (!reader.readU16(value.sourceLogicalAddress) || !reader.readU16(value.targetLogicalAddress) ||
        !reader.readU32(value.transactionId)) {
        return Result<ApplicationMessageNack>::error(ProtocolErrorCode::TruncatedPayload);
    }
    std::uint8_t code = 0;
    if (!reader.readU8(code) || code < 0x01 || code > 0x05) {
        return Result<ApplicationMessageNack>::error(ProtocolErrorCode::InvalidField, code);
    }
    value.code = static_cast<ApplicationNackCode>(code);
    return Result<ApplicationMessageNack>::ok(value);
}

ByteBuffer encodeApplicationMessageNack(const ApplicationMessageNack &value) noexcept {
    ByteBuffer out;
    out.reserve(9);
    WireWriter w{out};
    w.writeU16(value.sourceLogicalAddress);
    w.writeU16(value.targetLogicalAddress);
    w.writeU32(value.transactionId);
    w.writeU8(static_cast<std::uint8_t>(value.code));
    return out;
}

} // namespace uwb::protocol
