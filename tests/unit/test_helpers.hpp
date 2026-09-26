#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "uwb/protocol/bytes.hpp"

namespace uwb::test {

template <std::size_t N>
[[nodiscard]] uwb::protocol::ConstBytes view(const std::array<std::uint8_t, N> &v) noexcept {
    return uwb::protocol::ConstBytes{v.data(), N};
}

[[nodiscard]] inline std::string hex(uwb::protocol::ConstBytes bytes) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size() * 3);
    for (std::uint8_t byte : bytes) {
        out.push_back(digits[byte >> 4U]);
        out.push_back(digits[byte & 0x0FU]);
        out.push_back(' ');
    }
    return out;
}

[[nodiscard]] inline bool sameBytes(uwb::protocol::ConstBytes a, uwb::protocol::ConstBytes b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

} // namespace uwb::test
