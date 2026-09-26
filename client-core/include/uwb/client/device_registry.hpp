#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "uwb/client/client_connection.hpp"
#include "uwb/client/client_types.hpp"
#include "uwb/domain/device_identity.hpp"
#include "uwb/domain/uwb_configuration.hpp"
#include "uwb/protocol/payloads.hpp"

namespace uwb::client {

// One discovered device (specification §58).
struct DeviceEntry {
    domain::DeviceUuid uuid;              // registry key (§29)
    uwb::protocol::DeviceIdResponse identity;
    Endpoint discoveryEndpoint; // UDP endpoint the device answered from
    Endpoint tcpEndpoint;       // where the TCP connection is opened
    std::uint64_t firstSeenUs = 0;
    std::uint64_t lastSeenUs = 0;
    std::uint32_t sightings = 0;
    ConnectionState connectionState = ConnectionState::Disconnected;
    bool managed = false; // a connection object exists for this device
    std::shared_ptr<ClientConnection> connection;

    // Last known device state, used to populate the CLI without re-reading.
    std::optional<domain::UwbConfiguration> configuration;
    std::optional<uwb::protocol::ConnectionStatusRecord> status;
};

// Discovery registry keyed by Device UUID (§29 acceptance criterion).
class DeviceRegistry {
public:
    explicit DeviceRegistry(std::uint32_t staleTimeoutMs) noexcept;

    // Adds the device or refreshes endpoint / lastSeen for an existing UUID.
    void updateSeen(const uwb::protocol::DeviceIdResponse &identity, const Endpoint &discoveryEndpoint,
                    std::uint64_t nowUs);

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool contains(const domain::DeviceUuid &uuid) const noexcept { return entries_.count(uuid) != 0; }

    [[nodiscard]] const DeviceEntry *find(const domain::DeviceUuid &uuid) const noexcept;
    [[nodiscard]] DeviceEntry *find(const domain::DeviceUuid &uuid) noexcept;
    [[nodiscard]] const DeviceEntry *byLogicalAddress(std::uint16_t address) const noexcept;
    [[nodiscard]] const DeviceEntry *byName(const std::string &name) const noexcept;

    [[nodiscard]] std::vector<const DeviceEntry *> all() const noexcept;
    [[nodiscard]] std::vector<const DeviceEntry *> connected() const noexcept;

    void attachConnection(const domain::DeviceUuid &uuid, std::shared_ptr<ClientConnection> connection);
    void detachConnection(const domain::DeviceUuid &uuid);
    void setConnectionState(const domain::DeviceUuid &uuid, ConnectionState state);

    void setConfiguration(const domain::DeviceUuid &uuid, const domain::UwbConfiguration &configuration);
    void setStatus(const domain::DeviceUuid &uuid, const uwb::protocol::ConnectionStatusRecord &status);

    // Removes devices that have not been seen recently and have no connection.
    std::size_t pruneStale(std::uint64_t nowUs);

    [[nodiscard]] std::uint32_t staleTimeoutMs() const noexcept { return staleTimeoutMs_; }
    void setStaleTimeoutMs(std::uint32_t timeoutMs) noexcept { staleTimeoutMs_ = timeoutMs; }

private:
    std::uint32_t staleTimeoutMs_;
    std::map<domain::DeviceUuid, DeviceEntry> entries_;
};

} // namespace uwb::client
