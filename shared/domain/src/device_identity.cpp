#include "uwb/domain/device_identity.hpp"

namespace uwb::domain {

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

[[nodiscard]] std::optional<std::uint8_t> hexNibble(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return static_cast<std::uint8_t>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<std::uint8_t>(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
        return static_cast<std::uint8_t>(c - 'A' + 10);
    }
    return std::nullopt;
}

} // namespace

std::string uuidToString(const DeviceUuid &uuid) noexcept {
    char buffer[37] = {};
    uuid.formatLowerHex(buffer, sizeof(buffer));
    return std::string(buffer);
}

std::optional<DeviceUuid> uuidFromText(std::string_view text) noexcept {
    auto parsed = uwb::protocol::uuidFromString(text);
    if (parsed.ok()) {
        return parsed.value();
    }

    // Accept 32 bare hex digits as well; CLI users often paste them without dashes.
    DeviceUuid uuid;
    std::size_t index = 0;
    for (const char c : text) {
        if (c == '-') {
            continue;
        }
        const auto nibble = hexNibble(c);
        if (!nibble.has_value() || index >= DeviceUuid::kSize * 2) {
            return std::nullopt;
        }
        const std::size_t byteIndex = index / 2;
        const std::uint8_t value = *nibble;
        if ((index % 2) == 0) {
            uuid.bytes[byteIndex] = static_cast<std::uint8_t>(value << 4);
        } else {
            uuid.bytes[byteIndex] = static_cast<std::uint8_t>(uuid.bytes[byteIndex] | value);
        }
        ++index;
    }
    if (index != DeviceUuid::kSize * 2) {
        return std::nullopt;
    }
    return uuid;
}

std::string boardIdToHex(const BoardId &boardId) noexcept {
    std::string out;
    out.reserve(boardId.size() * 2);
    for (const std::uint8_t byte : boardId) {
        out.push_back(kHexDigits[(byte >> 4U) & 0x0FU]);
        out.push_back(kHexDigits[byte & 0x0FU]);
    }
    return out;
}

DeviceUuid uuidFromBoardId(const BoardId &boardId) noexcept {
    auto uuid = uwb::protocol::deviceUuidFromBoardId(uwb::protocol::ConstBytes{boardId.data(), boardId.size()});
    return uuid.ok() ? uuid.value() : DeviceUuid{};
}

} // namespace uwb::domain
