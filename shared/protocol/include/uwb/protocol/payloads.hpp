#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "uwb/protocol/bytes.hpp"
#include "uwb/protocol/capabilities.hpp"
#include "uwb/protocol/constants.hpp"
#include "uwb/protocol/dids.hpp"
#include "uwb/protocol/result.hpp"
#include "uwb/protocol/service_ids.hpp"
#include "uwb/protocol/uuid.hpp"

namespace uwb::protocol {

// ---------------------------------------------------------------------------
// 0x0000 Generic Header NACK (specification §15)
// ---------------------------------------------------------------------------
struct GenericHeaderNack {
    GenericHeaderNackCode code = GenericHeaderNackCode::IncorrectHeaderPattern;
};

[[nodiscard]] Result<GenericHeaderNack> decodeGenericHeaderNack(ConstBytes payload) noexcept;
[[nodiscard]] ByteBuffer encodeGenericHeaderNack(const GenericHeaderNack &value) noexcept;

// ---------------------------------------------------------------------------
// 0x0001 Device Identification Request (specification §16)
// ---------------------------------------------------------------------------
struct DeviceIdRequest {};

[[nodiscard]] Result<DeviceIdRequest> decodeDeviceIdRequest(ConstBytes payload) noexcept;
[[nodiscard]] ByteBuffer encodeDeviceIdRequest(const DeviceIdRequest &value) noexcept;

// ---------------------------------------------------------------------------
// 0x0004 Device Identification Response (specification §17)
// ---------------------------------------------------------------------------
struct DeviceIdResponse {
    Uuid deviceUuid;
    std::uint16_t logicalAddress = kLogicalAddressInvalid;
    std::uint16_t tcpPort = kDefaultProtocolPort;
    CapabilityMask capabilityMask = 0;
    ControlStatus controlStatus = ControlStatus::Available;
    std::uint8_t activeObservers = 0;
    std::uint8_t maxObservers = 0;
    std::string deviceName; // UTF-8, 0..32 bytes, not NUL terminated
};

[[nodiscard]] Result<DeviceIdResponse> decodeDeviceIdResponse(ConstBytes payload) noexcept;
[[nodiscard]] ByteBuffer encodeDeviceIdResponse(const DeviceIdResponse &value) noexcept;

// ---------------------------------------------------------------------------
// 0x0005 / 0x0006 Connection Activation (specification §18)
// ---------------------------------------------------------------------------
struct ConnectionActivationRequest {
    std::uint16_t clientLogicalAddress = kLogicalAddressInvalid;
    ConnectionRole requestedRole = ConnectionRole::Observer;
    std::uint8_t flags = 0; // zero in v1
    Uuid clientInstanceUuid; // zero UUID permitted for anonymous/test clients
};

[[nodiscard]] Result<ConnectionActivationRequest> decodeConnectionActivationRequest(ConstBytes payload) noexcept;
[[nodiscard]] ByteBuffer encodeConnectionActivationRequest(const ConnectionActivationRequest &value) noexcept;

struct ConnectionActivationResponse {
    std::uint16_t serverLogicalAddress = kLogicalAddressInvalid;
    ConnectionRole assignedRole = ConnectionRole::None; // zero if rejected
    ActivationResponseCode responseCode = ActivationResponseCode::AcceptedRequestedRole;
    Uuid deviceUuid;
    std::uint32_t serverMaxPayload = kMaxProtocolPayload;
    CapabilityMask capabilityMask = 0;
};

[[nodiscard]] Result<ConnectionActivationResponse> decodeConnectionActivationResponse(ConstBytes payload) noexcept;
[[nodiscard]] ByteBuffer encodeConnectionActivationResponse(const ConnectionActivationResponse &value) noexcept;

// ---------------------------------------------------------------------------
// 0x0007 / 0x0008 Alive Check (specification §19)
// ---------------------------------------------------------------------------
struct AliveCheckRequest {
    std::uint64_t nonce = 0;
};

struct AliveCheckResponse {
    std::uint64_t nonce = 0;
};

[[nodiscard]] Result<AliveCheckRequest> decodeAliveCheckRequest(ConstBytes payload) noexcept;
[[nodiscard]] ByteBuffer encodeAliveCheckRequest(const AliveCheckRequest &value) noexcept;
[[nodiscard]] Result<AliveCheckResponse> decodeAliveCheckResponse(ConstBytes payload) noexcept;
[[nodiscard]] ByteBuffer encodeAliveCheckResponse(const AliveCheckResponse &value) noexcept;

// ---------------------------------------------------------------------------
// 0x8001 Application Message envelope (specification §20.1)
// ---------------------------------------------------------------------------
struct ApplicationEnvelope {
    std::uint16_t sourceLogicalAddress = kLogicalAddressInvalid;
    std::uint16_t targetLogicalAddress = kLogicalAddressInvalid;
    std::uint32_t transactionId = 0; // zero is reserved for non-transactional messages
    std::uint8_t flags = 0;
    ByteBuffer servicePdu;

    [[nodiscard]] ConstBytes serviceBytes() const noexcept { return bytesOf(servicePdu); }
    [[nodiscard]] bool ackRequired() const noexcept { return (flags & kApplicationFlagAckRequired) != 0; }
};

[[nodiscard]] Result<ApplicationEnvelope> decodeApplicationEnvelope(ConstBytes payload) noexcept;
[[nodiscard]] ByteBuffer encodeApplicationEnvelope(const ApplicationEnvelope &value) noexcept;

// ---------------------------------------------------------------------------
// 0x8002 / 0x8003 Application Message ACK / NACK (specification §20.2, §20.3)
// ---------------------------------------------------------------------------
struct ApplicationMessageAck {
    std::uint16_t sourceLogicalAddress = kLogicalAddressInvalid;
    std::uint16_t targetLogicalAddress = kLogicalAddressInvalid;
    std::uint32_t transactionId = 0;
};

struct ApplicationMessageNack {
    std::uint16_t sourceLogicalAddress = kLogicalAddressInvalid;
    std::uint16_t targetLogicalAddress = kLogicalAddressInvalid;
    std::uint32_t transactionId = 0; // zero if unavailable
    ApplicationNackCode code = ApplicationNackCode::InvalidEnvelopeLength;
};

[[nodiscard]] Result<ApplicationMessageAck> decodeApplicationMessageAck(ConstBytes payload) noexcept;
[[nodiscard]] ByteBuffer encodeApplicationMessageAck(const ApplicationMessageAck &value) noexcept;
[[nodiscard]] Result<ApplicationMessageNack> decodeApplicationMessageNack(ConstBytes payload) noexcept;
[[nodiscard]] ByteBuffer encodeApplicationMessageNack(const ApplicationMessageNack &value) noexcept;

} // namespace uwb::protocol
