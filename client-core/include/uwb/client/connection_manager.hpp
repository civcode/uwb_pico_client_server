#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "uwb/client/client_clock.hpp"
#include "uwb/client/client_connection.hpp"
#include "uwb/client/client_error.hpp"
#include "uwb/client/client_transport.hpp"
#include "uwb/client/client_types.hpp"
#include "uwb/domain/device_identity.hpp"

namespace uwb::client {

// Owns the set of client connections and their reconnect/address bookkeeping
// (specification §56). All methods run on the network I/O thread except
// connections()/count(), which return snapshots.
class ConnectionManager {
public:
    using SetupHook = std::function<void(ClientConnection &)>;
    using EventHook = std::function<void(const ClientConnection &, const EventDelivery &)>;
    using StateHook = std::function<void(const ClientConnection &)>;
    using ErrorHook = std::function<void(const ClientConnection &, const ClientError &)>;
    using DeviceNameResolver = std::function<std::string(const domain::DeviceUuid &)>;

    ConnectionManager(const ClientOptions &options, IClock &clock, ITaskExecutor &executor,
                      IClientTransportFactory &factory);

    void setHooks(SetupHook setup, EventHook events, StateHook state, ErrorHook errors);
    void setDeviceNameResolver(DeviceNameResolver resolver);

    // Engine-thread commands.
    ClientResult<std::shared_ptr<ClientConnection>> open(const domain::DeviceUuid &uuid, const Endpoint &endpoint);
    void close(const domain::DeviceUuid &uuid);
    void closeAll();
    void tick(std::uint64_t nowUs);

    // Engine-thread lookups.
    [[nodiscard]] std::shared_ptr<ClientConnection> find(const domain::DeviceUuid &uuid) const;
    [[nodiscard]] std::shared_ptr<ClientConnection> findByAddress(std::uint16_t clientLogicalAddress) const;

    [[nodiscard]] std::size_t count() const noexcept { return managed_.size(); }
    [[nodiscard]] std::size_t activeCount() const;
    [[nodiscard]] std::vector<std::shared_ptr<ClientConnection>> connections() const;
    [[nodiscard]] const ClientOptions &options() const noexcept { return options_; }

private:
    // Stored behind unique_ptr so that callback captures stay valid when the
    // manager vector grows.
    struct Managed {
        domain::DeviceUuid uuid;
        Endpoint endpoint;
        std::uint16_t clientLogicalAddress = 0;
        std::shared_ptr<ClientConnection> connection;
    };

    [[nodiscard]] std::uint16_t allocateAddress();
    void installCallbacks(Managed &entry);
    void reconnectFor(const domain::DeviceUuid &uuid);
    [[nodiscard]] Managed *lookup(const domain::DeviceUuid &uuid) noexcept;

    ClientOptions options_;
    IClock &clock_;
    ITaskExecutor &executor_;
    IClientTransportFactory &factory_;

    SetupHook setupHook_;
    EventHook eventHook_;
    StateHook stateHook_;
    ErrorHook errorHook_;
    DeviceNameResolver nameResolver_;

    std::vector<std::unique_ptr<Managed>> managed_;
    std::uint16_t nextAddress_;
};

} // namespace uwb::client
