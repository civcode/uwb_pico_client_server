#pragma once

#include <cstdint>
#include <vector>

#include "uwb/protocol/bytes.hpp"
#include "uwb/protocol/dids.hpp"
#include "uwb/protocol/result.hpp"

namespace uwb::protocol {

// DID 0xF01F Complete UWB Configuration (specification §40.22):
//
//   schemaVersion:u16
//   entryCount:u16
//   repeated entryCount times: did:u16 | length:u16 | data[length]
//
// It is an explicit DID/TLV aggregation, never a native struct dump.
inline constexpr std::uint16_t kCompleteConfigSchemaVersion1 = 1;
inline constexpr std::size_t kCompleteConfigHeaderSize = 4;

struct DidRecordEntry {
    std::uint16_t did = 0;
    ByteBuffer data;

    [[nodiscard]] ConstBytes dataBytes() const noexcept { return bytesOf(data); }
    [[nodiscard]] Did didId() const noexcept { return static_cast<Did>(did); }
};

struct CompleteUwbConfig {
    std::uint16_t schemaVersion = kCompleteConfigSchemaVersion1;
    std::vector<DidRecordEntry> entries;
};

// DIDs that v1 complete configuration entries may reference.
[[nodiscard]] constexpr bool isCompleteConfigDid(std::uint16_t value) noexcept {
    return value >= 0xF010 && value <= 0xF014;
}

[[nodiscard]] Result<CompleteUwbConfig> decodeCompleteUwbConfig(ConstBytes data) noexcept;
[[nodiscard]] ByteBuffer encodeCompleteUwbConfig(const CompleteUwbConfig &config) noexcept;

// Transactional validation (specification §40.22 steps 2-4): unknown DID,
// duplicate DID, wrong record size, oversized record. Returns the first error.
[[nodiscard]] Result<bool> validateCompleteUwbConfig(const CompleteUwbConfig &config) noexcept;

[[nodiscard]] const DidRecordEntry *findCompleteConfigEntry(const CompleteUwbConfig &config, Did did) noexcept;

} // namespace uwb::protocol
