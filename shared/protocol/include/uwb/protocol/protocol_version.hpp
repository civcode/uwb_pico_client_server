#pragma once

#include <cstdint>

#include "uwb/protocol/errors.hpp"
#include "uwb/protocol/result.hpp"

namespace uwb::protocol {

// Specification §12: bits 7..4 = major, bits 3..0 = minor.
inline constexpr std::uint8_t kProtocolMajorV1 = 1;
inline constexpr std::uint8_t kProtocolMinorV1 = 0;
inline constexpr std::uint8_t kProtocolVersionV1_0 = 0x10;

[[nodiscard]] constexpr std::uint8_t protocolMajor(std::uint8_t versionByte) noexcept {
    return static_cast<std::uint8_t>(versionByte >> 4U);
}

[[nodiscard]] constexpr std::uint8_t protocolMinor(std::uint8_t versionByte) noexcept {
    return static_cast<std::uint8_t>(versionByte & 0x0FU);
}

[[nodiscard]] constexpr std::uint8_t makeProtocolVersion(std::uint8_t major, std::uint8_t minor) noexcept {
    return static_cast<std::uint8_t>(((major & 0x0FU) << 4U) | (minor & 0x0FU));
}

[[nodiscard]] constexpr std::uint8_t inverseProtocolVersion(std::uint8_t versionByte) noexcept {
    return static_cast<std::uint8_t>(~versionByte);
}

[[nodiscard]] constexpr bool inverseVersionMatches(std::uint8_t versionByte, std::uint8_t inverseByte) noexcept {
    return inverseProtocolVersion(versionByte) == inverseByte;
}

// Specification §12 compatibility rules.
[[nodiscard]] constexpr bool majorCompatible(std::uint8_t remoteVersionByte) noexcept {
    return protocolMajor(remoteVersionByte) == kProtocolMajorV1;
}

// Same major and remote minor <= local minor: fully accepted.
// Same major and remote minor >  local minor: framing accepted, feature support decides.
[[nodiscard]] constexpr bool sameOrLowerMinor(std::uint8_t remoteVersionByte) noexcept {
    return majorCompatible(remoteVersionByte) && protocolMinor(remoteVersionByte) <= kProtocolMinorV1;
}

// Validate version/inverse bytes of a generic header (specification §13 validation order).
[[nodiscard]] Result<std::uint8_t> validateVersionBytes(std::uint8_t versionByte,
                                                        std::uint8_t inverseByte) noexcept;

} // namespace uwb::protocol
