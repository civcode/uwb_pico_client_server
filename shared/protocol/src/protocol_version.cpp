#include "uwb/protocol/protocol_version.hpp"

namespace uwb::protocol {

Result<std::uint8_t> validateVersionBytes(std::uint8_t versionByte, std::uint8_t inverseByte) noexcept {
    if (!inverseVersionMatches(versionByte, inverseByte)) {
        return Result<std::uint8_t>::error(ProtocolErrorCode::InverseVersionMismatch, versionByte);
    }
    if (!majorCompatible(versionByte)) {
        return Result<std::uint8_t>::error(ProtocolErrorCode::UnsupportedMajorVersion, versionByte);
    }
    return Result<std::uint8_t>::ok(versionByte);
}

} // namespace uwb::protocol
