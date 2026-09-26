#pragma once

#include <cstdint>
#include <vector>

#include "uwb/protocol/bytes.hpp"
#include "uwb/protocol/result.hpp"

namespace uwb::protocol {

// Generic TLV record used by versioned containers.
//
// Specification §40.21 (DID 0xF017): type:u16 | length:u16 | bytes...
// Records are delimited by the enclosing payload length; there is no explicit
// end-of-list marker.
struct TlvRecord {
    std::uint16_t type = 0;
    ByteBuffer value;

    [[nodiscard]] ConstBytes valueBytes() const noexcept { return bytesOf(value); }
};

inline constexpr std::size_t kTlvHeaderSize = 4;

[[nodiscard]] Result<std::vector<TlvRecord>> decodeTlvRecords(ConstBytes data) noexcept;
[[nodiscard]] ByteBuffer encodeTlvRecords(const std::vector<TlvRecord> &records) noexcept;

[[nodiscard]] const TlvRecord *findTlvRecord(const std::vector<TlvRecord> &records, std::uint16_t type) noexcept;

// TLV types of DID 0xF017 (specification §40.21).
enum class BackendInfoTlvType : std::uint16_t {
    DecaVersionText = 0x0001,
    DListText = 0x0002,
    KListText = 0x0003,
};

[[nodiscard]] constexpr bool isKnownBackendInfoTlvType(std::uint16_t value) noexcept {
    return value >= 0x0001 && value <= 0x0003;
}

[[nodiscard]] const char *toString(BackendInfoTlvType type) noexcept;

// Versioned container of DID 0xF017: schemaVersion:u16 followed by TLV records.
inline constexpr std::uint16_t kBackendInfoSchemaVersion1 = 1;

struct BackendInfoContainer {
    std::uint16_t schemaVersion = kBackendInfoSchemaVersion1;
    std::vector<TlvRecord> records;
};

[[nodiscard]] Result<BackendInfoContainer> decodeBackendInfo(ConstBytes data) noexcept;
[[nodiscard]] ByteBuffer encodeBackendInfo(const BackendInfoContainer &container) noexcept;

} // namespace uwb::protocol
