#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace uwb::protocol {

// Byte container/buffer conventions shared by protocol, server-core, client-core
// and firmware code (implementation plan §5.5).
using ByteBuffer = std::vector<std::uint8_t>;
using ConstBytes = std::span<const std::uint8_t>;

inline ConstBytes bytesOf(const ByteBuffer &buffer) noexcept {
    return ConstBytes{buffer.data(), buffer.size()};
}

template <std::size_t N>
inline ConstBytes bytesOf(const std::array<std::uint8_t, N> &arr) noexcept {
    return ConstBytes{arr.data(), N};
}

} // namespace uwb::protocol
