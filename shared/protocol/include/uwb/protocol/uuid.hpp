#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "uwb/protocol/bytes.hpp"
#include "uwb/protocol/result.hpp"

namespace uwb::protocol {

// 16-byte UUID in network byte order (specification §11.2).
struct Uuid {
    static constexpr std::size_t kSize = 16;

    std::array<std::uint8_t, kSize> bytes{};

    [[nodiscard]] bool isZero() const noexcept;
    [[nodiscard]] ConstBytes asBytes() const noexcept { return ConstBytes{bytes.data(), bytes.size()}; }

    // Lowercase dashed form, e.g. "18659371-d91d-42d0-a58d-13b10305bbee".
    // out must hold 37 bytes (36 characters + NUL).
    void formatLowerHex(char *out, std::size_t outSize) const noexcept;

    [[nodiscard]] bool operator==(const Uuid &other) const noexcept { return bytes == other.bytes; }
    [[nodiscard]] bool operator!=(const Uuid &other) const noexcept { return !(*this == other); }
    [[nodiscard]] bool operator<(const Uuid &other) const noexcept { return bytes < other.bytes; }
};

[[nodiscard]] Result<Uuid> uuidFromString(std::string_view text) noexcept;
[[nodiscard]] Uuid uuidFromU64BigEndian(std::uint64_t value) noexcept;

// Project namespace UUID for Device UUID derivation (specification §6.2).
[[nodiscard]] const Uuid &deviceUuidNamespace() noexcept;

// RFC 9562 name-based UUID (SHA-1).
[[nodiscard]] Uuid uuidV5(const Uuid &namespaceUuid, ConstBytes name) noexcept;

// Canonical Device UUID derivation: the board ID bytes in the exact order returned
// by pico_get_unique_board_id(), rendered as 16 lowercase hexadecimal ASCII
// characters, fed to UUIDv5 with the project namespace.
[[nodiscard]] Result<Uuid> deviceUuidFromBoardId(ConstBytes boardId) noexcept;

// Convenience overload for host tools/simulators: the 8 board bytes in big-endian
// order (id[0] = most significant byte).
[[nodiscard]] Uuid deviceUuidFromBoardIdU64(std::uint64_t boardId) noexcept;

} // namespace uwb::protocol
