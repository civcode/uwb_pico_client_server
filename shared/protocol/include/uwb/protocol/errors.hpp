#pragma once

#include <cstdint>
#include <optional>

namespace uwb::protocol {

// Generic Header NACK codes (specification §15).
enum class GenericHeaderNackCode : std::uint8_t {
    IncorrectHeaderPattern = 0x00,
    UnknownPayloadType = 0x01,
    MessageTooLarge = 0x02,
    OutOfResources = 0x03,
    InvalidPayloadLength = 0x04,
    UnsupportedMajorVersion = 0x05,
    InvalidInCurrentState = 0x06,
};

// Application Message negative transport ACK codes (specification §20.3).
enum class ApplicationNackCode : std::uint8_t {
    InvalidSourceOrTargetAddress = 0x01,
    InvalidEnvelopeLength = 0x02,
    ConnectionNotActivated = 0x03,
    ResourceTemporarilyUnavailable = 0x04,
    InvalidEnvelopeFlags = 0x05,
};

// UDS-inspired negative response codes (specification §21.4).
enum class ServiceNrc : std::uint8_t {
    GeneralReject = 0x10,
    ServiceNotSupported = 0x11,
    SubFunctionNotSupported = 0x12,
    IncorrectMessageLengthOrInvalidFormat = 0x13,
    BusyRepeatRequest = 0x21,
    ConditionsNotCorrect = 0x22,
    RequestSequenceError = 0x24,
    RequestOutOfRange = 0x31,
    SecurityAccessDenied = 0x33,
    InvalidKey = 0x35,
    ExceedNumberOfAttempts = 0x36,
    RequiredTimeDelayNotExpired = 0x37,
    GeneralProgrammingFailure = 0x72,
    ResponsePending = 0x78,
    SubFunctionNotSupportedInActiveSession = 0x7E,
    ServiceNotSupportedInActiveSession = 0x7F,
};

// Framing/decoding failures of the protocol library itself.
//
// These are intentionally distinct from GenericHeaderNackCode (transport header
// problems), ApplicationNackCode (envelope problems) and ServiceNrc (service
// problems) — specification §21.4, implementation plan §10.8.
enum class ProtocolErrorCode : std::uint16_t {
    None = 0,

    // Header framing (specification §13)
    NeedMoreData = 1,
    HeaderTooShort = 2,
    InverseVersionMismatch = 3,
    UnsupportedMajorVersion = 4,
    UnknownPayloadType = 5,
    PayloadTooLarge = 6,
    InvalidPayloadLength = 7,

    // Payload decoding
    TruncatedPayload = 8,
    UnexpectedPayloadLength = 9,
    InvalidField = 10,
    InvalidReservedBits = 11,
    DuplicateEntry = 12,

    // Service framing
    EmptyServicePdu = 13,
    UnknownServiceId = 14,
    UnsupportedService = 15,

    // Records and schemas
    UnsupportedSchemaVersion = 16,
    InvalidChecksum = 18,
};

struct ProtocolError {
    ProtocolErrorCode code = ProtocolErrorCode::None;
    std::uint32_t detail = 0;

    constexpr explicit operator bool() const noexcept { return code != ProtocolErrorCode::None; }
};

[[nodiscard]] inline constexpr ProtocolError makeError(ProtocolErrorCode code, std::uint32_t detail = 0) noexcept {
    return ProtocolError{code, detail};
}

inline constexpr bool isTransient(ProtocolErrorCode code) noexcept {
    return code == ProtocolErrorCode::NeedMoreData;
}

// Map a framing error to the Generic Header NACK code the server should send
// before closing a TCP connection (specification §13, §15).
inline constexpr std::optional<GenericHeaderNackCode> genericHeaderNackFor(ProtocolErrorCode code) noexcept {
    switch (code) {
    case ProtocolErrorCode::InverseVersionMismatch:
        return GenericHeaderNackCode::IncorrectHeaderPattern;
    case ProtocolErrorCode::HeaderTooShort:
        return GenericHeaderNackCode::IncorrectHeaderPattern;
    case ProtocolErrorCode::UnknownPayloadType:
        return GenericHeaderNackCode::UnknownPayloadType;
    case ProtocolErrorCode::PayloadTooLarge:
        return GenericHeaderNackCode::MessageTooLarge;
    case ProtocolErrorCode::InvalidPayloadLength:
        return GenericHeaderNackCode::InvalidPayloadLength;
    case ProtocolErrorCode::UnsupportedMajorVersion:
        return GenericHeaderNackCode::UnsupportedMajorVersion;
    case ProtocolErrorCode::NeedMoreData:
    case ProtocolErrorCode::TruncatedPayload:
    case ProtocolErrorCode::UnexpectedPayloadLength:
    case ProtocolErrorCode::InvalidField:
    case ProtocolErrorCode::InvalidReservedBits:
    case ProtocolErrorCode::DuplicateEntry:
    case ProtocolErrorCode::EmptyServicePdu:
    case ProtocolErrorCode::UnknownServiceId:
    case ProtocolErrorCode::UnsupportedService:
    case ProtocolErrorCode::UnsupportedSchemaVersion:
    case ProtocolErrorCode::InvalidChecksum:
    case ProtocolErrorCode::None:
        return std::nullopt;
    }
    return std::nullopt;
}

// Severe framing errors close the TCP connection after the NACK (specification §13).
// Errors that are payload/service level are handled with service NRCs instead.
inline constexpr bool requiresConnectionClose(ProtocolErrorCode code) noexcept {
    return genericHeaderNackFor(code).has_value();
}

} // namespace uwb::protocol
