#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "uwb/protocol/bytes.hpp"
#include "uwb/protocol/errors.hpp"

namespace uwb::protocol {

// Big-endian (network byte order) reader over a byte span.
//
// Specification §11.1: all multi-byte integers are big-endian, two's complement for
// signed values, IEEE-754 binary32 bits for f32. Native struct memcpy is never used.
class WireReader {
public:
    explicit WireReader(ConstBytes bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] std::size_t consumed() const noexcept { return offset_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }
    [[nodiscard]] bool exhausted() const noexcept { return offset_ == bytes_.size(); }
    [[nodiscard]] ConstBytes remainingBytes() const noexcept { return bytes_.subspan(offset_); }

    [[nodiscard]] bool readU8(std::uint8_t &out) noexcept { return readRaw(1) && assign(out); }

    [[nodiscard]] bool readU16(std::uint16_t &out) noexcept {
        return readRaw(2) && assignBigEndian(out);
    }
    [[nodiscard]] bool readU32(std::uint32_t &out) noexcept {
        return readRaw(4) && assignBigEndian(out);
    }
    [[nodiscard]] bool readU64(std::uint64_t &out) noexcept {
        return readRaw(8) && assignBigEndian(out);
    }
    [[nodiscard]] bool readI16(std::int16_t &out) noexcept {
        std::uint16_t raw = 0;
        if (!readU16(raw)) {
            return false;
        }
        out = static_cast<std::int16_t>(raw);
        return true;
    }
    [[nodiscard]] bool readI32(std::int32_t &out) noexcept {
        std::uint32_t raw = 0;
        if (!readU32(raw)) {
            return false;
        }
        out = static_cast<std::int32_t>(raw);
        return true;
    }
    [[nodiscard]] bool readI64(std::int64_t &out) noexcept {
        std::uint64_t raw = 0;
        if (!readU64(raw)) {
            return false;
        }
        out = static_cast<std::int64_t>(raw);
        return true;
    }
    [[nodiscard]] bool readF32(float &out) noexcept {
        std::uint32_t raw = 0;
        if (!readU32(raw)) {
            return false;
        }
        out = std::bit_cast<float>(raw);
        return true;
    }
    [[nodiscard]] bool readBytes(std::uint8_t *dst, std::size_t count) noexcept {
        if (!readRaw(count)) {
            return false;
        }
        std::memcpy(dst, bytes_.data() + (offset_ - count), count);
        return true;
    }

    // Zero-copy view of the next `count` bytes.
    [[nodiscard]] bool readSpan(ConstBytes &out, std::size_t count) noexcept {
        if (!readRaw(count)) {
            return false;
        }
        out = bytes_.subspan(offset_ - count, count);
        return true;
    }
    [[nodiscard]] bool skip(std::size_t count) noexcept { return readRaw(count); }

    template <std::size_t N>
    [[nodiscard]] bool readArray(std::array<std::uint8_t, N> &out) noexcept {
        return readBytes(out.data(), N);
    }

    [[nodiscard]] bool atEnd() const noexcept { return offset_ == bytes_.size(); }

private:
    [[nodiscard]] bool readRaw(std::size_t count) noexcept {
        if (count > remaining()) {
            return false;
        }
        offset_ += count;
        return true;
    }

    [[nodiscard]] bool assign(std::uint8_t &out) const noexcept {
        out = bytes_[offset_ - 1];
        return true;
    }

    template <typename Unsigned>
    [[nodiscard]] bool assignBigEndian(Unsigned &out) const noexcept {
        constexpr std::size_t width = sizeof(Unsigned);
        out = 0;
        for (std::size_t i = 0; i < width; ++i) {
            out = static_cast<Unsigned>(out << 8U);
            out = static_cast<Unsigned>(out | bytes_[offset_ - width + i]);
        }
        return true;
    }

    ConstBytes bytes_;
    std::size_t offset_ = 0;
};

// Free-function helpers for single-value big-endian conversion.
[[nodiscard]] std::uint16_t readU16BE(ConstBytes bytes, std::size_t offset = 0) noexcept;
[[nodiscard]] std::uint32_t readU32BE(ConstBytes bytes, std::size_t offset = 0) noexcept;
[[nodiscard]] std::uint64_t readU64BE(ConstBytes bytes, std::size_t offset = 0) noexcept;

} // namespace uwb::protocol
