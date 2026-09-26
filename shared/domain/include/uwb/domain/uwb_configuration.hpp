#pragma once

#include <cstdint>
#include <optional>

#include "uwb/protocol/complete_config.hpp"
#include "uwb/protocol/did_records.hpp"
#include "uwb/protocol/dids.hpp"

namespace uwb::domain {

// Current-known UWB configuration of a device (specification §40.22, §72).
using UwbConfiguration = uwb::protocol::CompleteUwbConfig;

// Typed views over a complete configuration. Missing or malformed entries
// degrade to std::nullopt instead of asserting: a device may legitimately not
// support 0xF013/0xF014.
[[nodiscard]] std::optional<uwb::protocol::UwbDeviceParametersRecord>
findDeviceParameters(const UwbConfiguration &config) noexcept;

[[nodiscard]] std::optional<uwb::protocol::UwbTwrParametersRecord> findTwrParameters(
    const UwbConfiguration &config) noexcept;

[[nodiscard]] std::optional<uwb::protocol::UwbPdoaParametersRecord> findPdoaParameters(
    const UwbConfiguration &config) noexcept;

[[nodiscard]] std::optional<std::uint8_t> findWorkMode(const UwbConfiguration &config) noexcept;

[[nodiscard]] std::optional<std::uint8_t> findMode(const UwbConfiguration &config) noexcept;

// Replaces (or inserts) one entry of a complete configuration. Used by the
// client so that "apply configuration" travels as 0xF01F rather than as a
// scattered set of individual DID writes.
[[nodiscard]] bool upsertConfigurationEntry(UwbConfiguration &config, std::uint16_t did,
                                            uwb::protocol::ByteBuffer data) noexcept;

} // namespace uwb::domain
