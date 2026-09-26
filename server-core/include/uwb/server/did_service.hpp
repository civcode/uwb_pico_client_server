#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "uwb/protocol/capabilities.hpp"
#include "uwb/protocol/did_records.hpp"
#include "uwb/protocol/dids.hpp"
#include "uwb/protocol/uuid.hpp"
#include "uwb/server/config_storage.hpp"
#include "uwb/server/connection_context.hpp"
#include "uwb/server/server_config.hpp"
#include "uwb/server/server_errors.hpp"
#include "uwb/server/transport.hpp"

namespace uwb::server {

using uwb::protocol::ByteBuffer;
using uwb::protocol::ConstBytes;
using uwb::protocol::Did;
using uwb::protocol::Uuid;

// DID framework (implementation plan §16.5, specification §26, §27, §39, §40).
//
// Pico-owned and runtime DIDs are answered by the server core itself. UWB-backed
// DIDs report "NeedsBackend" so the dispatcher can run them asynchronously
// through IUwbBackend. Record layouts and access/persistence classes come from
// the shared registry (uwb::protocol), never from local copies.
class DidService {
public:
    DidService(const ServerConfig &config, IConfigurationStorage &storage) noexcept;

    // --- static identity, set once at boot -----------------------------------
    void setDeviceIdentity(const Uuid &deviceUuid, const std::array<std::uint8_t, 8> &boardUniqueId,
                           std::string deviceName, std::uint16_t logicalAddress, std::uint16_t tcpPort) noexcept;
    void setFirmwareVersion(uwb::protocol::ServerFirmwareVersionRecord version) noexcept;
    void setCapabilityOverride(uwb::protocol::CapabilityOverrideRecord capabilityOverride) noexcept;

    // --- runtime values ------------------------------------------------------
    void setCapabilityMasks(uwb::protocol::CapabilityMask known, uwb::protocol::CapabilityMask detected) noexcept;
    void setTimeouts(TimeoutSettings timeouts) noexcept;
    void setUptime(std::uint64_t uptimeUs) noexcept;
    void setWifiRssi(std::int16_t rssiDbm) noexcept;
    void setLatestDistance(std::int32_t distanceMm) noexcept;
    void setLatestSensor(uwb::protocol::LatestSensorDataRecord record) noexcept;
    void setConnectionStatus(uwb::protocol::ConnectionStatusRecord status) noexcept;

    // Effective capabilities: detected support masked by the known mask, then
    // adjusted by the override record (specification §39.3, DID 0xF008).
    [[nodiscard]] uwb::protocol::CapabilityMask effectiveCapabilities() const noexcept;

    // --- read path (§26) ----------------------------------------------------
    // NeedsBackend: the value lives behind the UWB backend and the dispatcher has
    // to fetch it asynchronously (§26 async completion).
    [[nodiscard]] ServerResult<ByteBuffer> read(Did did) const;

    // --- write path (§27) ---------------------------------------------------
    // Validates structure (§40), access/persistence class (§39) and role/session
    // policy (§9.2, §22.3), then commits atomically. A rejected write leaves the
    // stored value untouched.
    [[nodiscard]] ServerResult<bool> write(ConnectionId connection, const ConnectionContext &ctx, Did did,
                                           ConstBytes record);

    [[nodiscard]] const TimeoutSettings &timeouts() const noexcept { return timeouts_; }
    [[nodiscard]] std::uint16_t logicalAddress() const noexcept { return logicalAddress_; }
    [[nodiscard]] std::uint16_t tcpPort() const noexcept { return tcpPort_; }
    [[nodiscard]] const Uuid &deviceUuid() const noexcept { return deviceUuid_; }
    [[nodiscard]] const std::string &deviceName() const noexcept { return deviceName_; }
    [[nodiscard]] std::uint32_t writeCount() const noexcept { return writes_; }
    [[nodiscard]] std::uint32_t rejectedWriteCount() const noexcept { return rejectedWrites_; }

private:
    [[nodiscard]] ServerStatus checkWritePolicy(Did did, const ConnectionContext &ctx) const;
    [[nodiscard]] ServerStatus checkRecord(Did did, ConstBytes record) const;

    const ServerConfig &config_;
    IConfigurationStorage &storage_;

    Uuid deviceUuid_{};
    std::array<std::uint8_t, 8> boardUniqueId_{};
    std::string deviceName_;
    std::uint16_t logicalAddress_ = 0;
    std::uint16_t tcpPort_ = 0;

    uwb::protocol::ServerFirmwareVersionRecord firmware_{};
    uwb::protocol::CapabilityOverrideRecord capabilityOverride_{};
    uwb::protocol::CapabilityMask knownMask_ = 0;
    uwb::protocol::CapabilityMask detectedMask_ = 0;
    uwb::protocol::ConnectionStatusRecord connectionStatus_{};
    uwb::protocol::LatestSensorDataRecord latestSensor_{};
    std::int32_t latestDistanceMm_ = 0;

    // Writes of persistent Pico DIDs that are answered by the core (0xF007,
    // 0xF009) are staged through IConfigurationStorage.
    TimeoutSettings timeouts_{};
    std::uint64_t uptimeUs_ = 0;
    std::int16_t wifiRssiDbm_ = 0;
    std::int32_t latestDistance_ = 0;

    std::uint32_t writes_ = 0;
    std::uint32_t rejectedWrites_ = 0;
};

} // namespace uwb::server
