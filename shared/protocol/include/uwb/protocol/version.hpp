#pragma once

#include <cstdint>
#include <string_view>

namespace uwb::protocol {

// Version of the persistent Pico<->client wire protocol (specification §74).
inline constexpr std::uint16_t kWireProtocolVersion = 1;

// Version of this protocol library.
std::string_view libraryVersion() noexcept;

} // namespace uwb::protocol
