#include "uwb/client/device_registry.hpp"

#include <utility>

namespace uwb::client {

namespace {

// The TCP port comes from the Device Id Response (§17); the host comes from the
// datagram source, which is the address the client actually reached.
[[nodiscard]] Endpoint tcpEndpointFor(const Endpoint &discoveryEndpoint, const uwb::protocol::DeviceIdResponse &identity) {
    Endpoint tcp;
    tcp.host = discoveryEndpoint.host;
    tcp.port = identity.tcpPort != 0 ? identity.tcpPort : uwb::protocol::kDefaultProtocolPort;
    return tcp;
}

} // namespace

DeviceRegistry::DeviceRegistry(std::uint32_t staleTimeoutMs) noexcept : staleTimeoutMs_(staleTimeoutMs) {}

void DeviceRegistry::updateSeen(const uwb::protocol::DeviceIdResponse &identity, const Endpoint &discoveryEndpoint,
                               std::uint64_t nowUs) {
    const auto uuid = domain::DeviceUuid{identity.deviceUuid};
    auto it = entries_.find(uuid);
    if (it == entries_.end()) {
        DeviceEntry entry;
        entry.uuid = uuid;
        entry.identity = identity;
        entry.discoveryEndpoint = discoveryEndpoint;
        entry.tcpEndpoint = tcpEndpointFor(discoveryEndpoint, identity);
        entry.firstSeenUs = nowUs;
        entry.lastSeenUs = nowUs;
        entry.sightings = 1;
        it = entries_.emplace(uuid, std::move(entry)).first;
        return;
    }

    DeviceEntry &entry = it->second;
    entry.uuid = uuid;
    entry.identity = identity;
    entry.lastSeenUs = nowUs;
    entry.sightings++;

    // Endpoint updates must be visible to the application, but a device that
    // already has a live TCP connection keeps the endpoint it connected to.
    if (entry.connectionState == ConnectionState::Disconnected) {
        entry.discoveryEndpoint = discoveryEndpoint;
        entry.tcpEndpoint = tcpEndpointFor(discoveryEndpoint, identity);
    }
}

const DeviceEntry *DeviceRegistry::find(const domain::DeviceUuid &uuid) const noexcept {
    auto it = entries_.find(uuid);
    return it == entries_.end() ? nullptr : &it->second;
}

DeviceEntry *DeviceRegistry::find(const domain::DeviceUuid &uuid) noexcept {
    auto it = entries_.find(uuid);
    return it == entries_.end() ? nullptr : &it->second;
}

const DeviceEntry *DeviceRegistry::byLogicalAddress(std::uint16_t address) const noexcept {
    for (const auto &pair : entries_) {
        if (pair.second.identity.logicalAddress == address) {
            return &pair.second;
        }
    }
    return nullptr;
}

const DeviceEntry *DeviceRegistry::byName(const std::string &name) const noexcept {
    for (const auto &pair : entries_) {
        if (pair.second.identity.deviceName == name) {
            return &pair.second;
        }
    }
    return nullptr;
}

std::vector<const DeviceEntry *> DeviceRegistry::all() const noexcept {
    std::vector<const DeviceEntry *> out;
    out.reserve(entries_.size());
    for (const auto &pair : entries_) {
        out.push_back(&pair.second);
    }
    return out;
}

std::vector<const DeviceEntry *> DeviceRegistry::connected() const noexcept {
    std::vector<const DeviceEntry *> out;
    for (const auto &pair : entries_) {
        if (pair.second.connection) {
            out.push_back(&pair.second);
        }
    }
    return out;
}

void DeviceRegistry::attachConnection(const domain::DeviceUuid &uuid, std::shared_ptr<ClientConnection> connection) {
    DeviceEntry &entry = entries_[uuid];
    if (entry.firstSeenUs == 0) {
        entry.identity.deviceUuid = uuid;
    }
    entry.connection = std::move(connection);
    entry.managed = true;
    entry.connectionState = entry.connection ? entry.connection->state() : ConnectionState::Disconnected;
}

void DeviceRegistry::detachConnection(const domain::DeviceUuid &uuid) {
    auto it = entries_.find(uuid);
    if (it == entries_.end()) {
        return;
    }
    it->second.connection.reset();
    it->second.managed = false;
    it->second.connectionState = ConnectionState::Disconnected;
}

void DeviceRegistry::setConnectionState(const domain::DeviceUuid &uuid, ConnectionState state) {
    auto it = entries_.find(uuid);
    if (it == entries_.end()) {
        return;
    }
    it->second.connectionState = state;
    // Once the connection has a real endpoint, the registry reports that one.
    // A connection that has not connected yet must not erase the discovered
    // endpoint, otherwise a reconnect would have no address to dial.
    if (it->second.connection && !it->second.connection->endpointHost().empty()) {
        it->second.tcpEndpoint =
            Endpoint{it->second.connection->endpointHost(), it->second.connection->endpointPort()};
    }
}

void DeviceRegistry::setConfiguration(const domain::DeviceUuid &uuid, const domain::UwbConfiguration &configuration) {
    auto it = entries_.find(uuid);
    if (it == entries_.end()) {
        return;
    }
    it->second.configuration = configuration;
}

void DeviceRegistry::setStatus(const domain::DeviceUuid &uuid, const uwb::protocol::ConnectionStatusRecord &status) {
    auto it = entries_.find(uuid);
    if (it == entries_.end()) {
        return;
    }
    it->second.status = status;
}

std::size_t DeviceRegistry::pruneStale(std::uint64_t nowUs) {
    std::size_t removed = 0;
    for (auto it = entries_.begin(); it != entries_.end();) {
        const bool stale = nowUs > it->second.lastSeenUs &&
                           nowUs - it->second.lastSeenUs > static_cast<std::uint64_t>(staleTimeoutMs_) * 1000ULL;
        if (stale && !it->second.connection) {
            it = entries_.erase(it);
            removed++;
            continue;
        }
        ++it;
    }
    return removed;
}

} // namespace uwb::client
