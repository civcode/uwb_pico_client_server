#pragma once

#include <cstdint>

#include "uwb/protocol/bytes.hpp"
#include "uwb/protocol/constants.hpp"
#include "uwb/protocol/payload_types.hpp"
#include "uwb/protocol/protocol_version.hpp"
#include "uwb/protocol/result.hpp"

namespace uwb::protocol {

// Specification §13.
struct GenericHeader {
    std::uint8_t protocolVersion = kProtocolVersionV1_0;
    PayloadType payloadType = PayloadType::GenericHeaderNack;
    std::uint32_t payloadLength = 0;
};

struct Frame {
    GenericHeader header;
    ByteBuffer payload;

    [[nodiscard]] PayloadType type() const noexcept { return header.payloadType; }
    [[nodiscard]] std::uint32_t payloadLength() const noexcept { return header.payloadLength; }
    [[nodiscard]] ConstBytes payloadBytes() const noexcept { return bytesOf(payload); }
};

// Decode exactly the 8-byte generic header, following the normative validation order.
[[nodiscard]] Result<GenericHeader> decodeGenericHeader(ConstBytes bytes) noexcept;

// Encode a generic header only (payload must be appended by the caller).
[[nodiscard]] ByteBuffer encodeGenericHeader(const GenericHeader &header) noexcept;

// Encode a complete frame, validating payload size.
[[nodiscard]] Result<ByteBuffer> encodeFrame(PayloadType type, ConstBytes payload,
                                            std::uint8_t protocolVersion = kProtocolVersionV1_0) noexcept;

} // namespace uwb::protocol
