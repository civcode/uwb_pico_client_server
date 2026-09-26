#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "uwb/protocol/uuid.hpp"

namespace uwb::domain {

// UI-independent identity types (specification §6, §78 shared/domain).
using DeviceUuid = uwb::protocol::Uuid;
using BoardId = std::array<std::uint8_t, 8>;

[[nodiscard]] std::string uuidToString(const DeviceUuid &uuid) noexcept;

// Accepts the canonical dashed form or 32 bare hex digits.
[[nodiscard]] std::optional<DeviceUuid> uuidFromText(std::string_view text) noexcept;

[[nodiscard]] std::string boardIdToHex(const BoardId &boardId) noexcept;

[[nodiscard]] DeviceUuid uuidFromBoardId(const BoardId &boardId) noexcept;

} // namespace uwb::domain
