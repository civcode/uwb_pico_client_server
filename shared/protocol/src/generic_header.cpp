#include "uwb/protocol/generic_header.hpp"

#include "uwb/protocol/protocol_version.hpp"
#include "uwb/protocol/wire_reader.hpp"
#include "uwb/protocol/wire_writer.hpp"

namespace uwb::protocol {

Result<GenericHeader> decodeGenericHeader(ConstBytes bytes) noexcept {
    if (bytes.size() < kGenericHeaderSize) {
        return Result<GenericHeader>::error(ProtocolErrorCode::HeaderTooShort,
                                            static_cast<std::uint32_t>(bytes.size()));
    }

    WireReader reader{bytes};

    std::uint8_t version = 0;
    std::uint8_t inverse = 0;
    std::uint16_t type = 0;
    std::uint32_t length = 0;

    if (!reader.readU8(version) || !reader.readU8(inverse) || !reader.readU16(type) || !reader.readU32(length)) {
        return Result<GenericHeader>::error(ProtocolErrorCode::HeaderTooShort);
    }

    if (auto check = validateVersionBytes(version, inverse); check.failed()) {
        return Result<GenericHeader>::error(check.error());
    }

    if (!isKnownPayloadType(type)) {
        return Result<GenericHeader>::error(ProtocolErrorCode::UnknownPayloadType, type);
    }

    if (length > kMaxProtocolPayload) {
        return Result<GenericHeader>::error(ProtocolErrorCode::PayloadTooLarge, length);
    }

    GenericHeader header;
    header.protocolVersion = version;
    header.payloadType = static_cast<PayloadType>(type);
    header.payloadLength = length;
    return Result<GenericHeader>::ok(header);
}

ByteBuffer encodeGenericHeader(const GenericHeader &header) noexcept {
    ByteBuffer out;
    out.reserve(kGenericHeaderSize);
    WireWriter w{out};
    w.writeU8(header.protocolVersion);
    w.writeU8(inverseProtocolVersion(header.protocolVersion));
    w.writeU16(static_cast<std::uint16_t>(header.payloadType));
    w.writeU32(header.payloadLength);
    return out;
}

Result<ByteBuffer> encodeFrame(PayloadType type, ConstBytes payload, std::uint8_t protocolVersion) noexcept {
    if (payload.size() > kMaxProtocolPayload) {
        return Result<ByteBuffer>::error(ProtocolErrorCode::PayloadTooLarge,
                                         static_cast<std::uint32_t>(payload.size()));
    }

    GenericHeader header;
    header.protocolVersion = protocolVersion;
    header.payloadType = type;
    header.payloadLength = static_cast<std::uint32_t>(payload.size());

    ByteBuffer out = encodeGenericHeader(header);
    out.insert(out.end(), payload.begin(), payload.end());
    return Result<ByteBuffer>::ok(std::move(out));
}

const char *toString(PayloadType type) noexcept {
    switch (type) {
    case PayloadType::GenericHeaderNack:
        return "GenericHeaderNack";
    case PayloadType::DeviceIdRequest:
        return "DeviceIdRequest";
    case PayloadType::DeviceIdResponse:
        return "DeviceIdResponse";
    case PayloadType::ConnectionActivationRequest:
        return "ConnectionActivationRequest";
    case PayloadType::ConnectionActivationResponse:
        return "ConnectionActivationResponse";
    case PayloadType::AliveCheckRequest:
        return "AliveCheckRequest";
    case PayloadType::AliveCheckResponse:
        return "AliveCheckResponse";
    case PayloadType::ApplicationMessage:
        return "ApplicationMessage";
    case PayloadType::ApplicationMessageAck:
        return "ApplicationMessageAck";
    case PayloadType::ApplicationMessageNack:
        return "ApplicationMessageNack";
    case PayloadType::EventNotification:
        return "EventNotification";
    }
    return "UnknownPayloadType";
}

} // namespace uwb::protocol
