#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "uwb/protocol/bytes.hpp"

namespace uwb::protocol {

// Big-endian writer appending to a ByteBuffer (specification §11.1).
class WireWriter {
public:
    explicit WireWriter(ByteBuffer &out) noexcept : out_(out) {}

    void writeU8(std::uint8_t value) noexcept {
        out_.push_back(value);
    }

    void writeU16(std::uint16_t value) noexcept {
        out_.push_back(static_cast<std::uint8_t>(value >> 8U));
        out_.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    }

    void writeU32(std::uint32_t value) noexcept {
        for (int shift = 24; shift >= 0; shift -= 8) {
            out_.push_back(static_cast<std::uint8_t>((value >> static_cast<unsigned>(shift)) & 0xFFU));
        }
    }

    void writeU64(std::uint64_t value) noexcept {
        for (int shift = 56; shift >= 0; shift -= 8) {
            out_.push_back(static_cast<std::uint8_t>((value >> static_cast<unsigned>(shift)) & 0xFFU));
        }
    }

    void writeI8(std::int8_t value) noexcept {
        writeU8(static_cast<std::uint8_t>(value));
    }
    void writeI16(std::int16_t value) noexcept { writeU16(static_cast<std::uint16_t>(value)); }
    void writeI32(std::int32_t value) noexcept { writeU32(static_cast<std::uint32_t>(value)); }
    void writeI64(std::int64_t value) noexcept { writeU64(static_cast<std::uint64_t>(value)); }

    void writeF32(float value) noexcept { writeU32(std::bit_cast<std::uint32_t>(value)); }

    void writeBytes(ConstBytes bytes) noexcept {
        out_.insert(out_.end(), bytes.begin(), bytes.end());
    }

    void writeArray(const std::array<std::uint8_t, 16> &bytes) noexcept {
        out_.insert(out_.end(), bytes.begin(), bytes.end());
    }

    // Length-prefixed UTF-8 string, no NUL termination (specification §11.2).
    void writeLengthPrefixedString8(std::string_view value) {
        writeU8(static_cast<std::uint8_t>(value.size()));
        writeBytes(ConstBytes{reinterpret_cast<const std::uint8_t *>(value.data()), value.size()});
    }

    [[nodiscard]] std::size_t size() const noexcept { return out_.size(); }

private:
    ByteBuffer &out_;
};

} // namespace uwb::protocol
