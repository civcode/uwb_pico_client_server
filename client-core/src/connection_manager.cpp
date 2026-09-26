#include "uwb/client/connection_manager.hpp"

#include <utility>

#include "uwb/protocol/constants.hpp"

namespace uwb::client {

ConnectionManager::ConnectionManager(const ClientOptions &options, IClock &clock, ITaskExecutor &executor,
                                     IClientTransportFactory &factory)
    : options_(options), clock_(clock), executor_(executor), factory_(factory),
      nextAddress_(options.clientLogicalAddressBase) {}

void ConnectionManager::setHooks(SetupHook setup, EventHook events, StateHook state, ErrorHook errors) {
    setupHook_ = std::move(setup);
    eventHook_ = std::move(events);
    stateHook_ = std::move(state);
    errorHook_ = std::move(errors);
}

void ConnectionManager::setDeviceNameResolver(DeviceNameResolver resolver) { nameResolver_ = std::move(resolver); }

ConnectionManager::Managed *ConnectionManager::lookup(const domain::DeviceUuid &uuid) noexcept {
    for (auto &holder : managed_) {
        if (holder->uuid == uuid) {
            return holder.get();
        }
    }
    return nullptr;
}

// Client logical addresses are allocated per device from the client range so
// that concurrent connections stay distinguishable on every device (§9, §39).
std::uint16_t ConnectionManager::allocateAddress() {
    const std::uint16_t first = options_.clientLogicalAddressBase;
    const std::uint16_t last = uwb::protocol::kClientLogicalAddressMax;
    const std::size_t span = static_cast<std::size_t>(last) - static_cast<std::size_t>(first) + 1ULL;

    for (std::size_t probe = 0; probe < span; ++probe) {
        const std::uint16_t candidate = nextAddress_;
        nextAddress_ = nextAddress_ == last ? first : static_cast<std::uint16_t>(nextAddress_ + 1U);
        if (candidate == 0U || candidate == uwb::protocol::kLogicalAddressBroadcast) {
            continue;
        }
        bool used = false;
        for (const auto &holder : managed_) {
            if (holder->clientLogicalAddress == candidate) {
                used = true;
                break;
            }
        }
        if (!used) {
            return candidate;
        }
    }
    return 0U;
}

void ConnectionManager::installCallbacks(Managed &entry) {
    ClientConnection *connection = entry.connection.get();

    connection->setSetupHandler([this](ClientConnection &target) {
        if (setupHook_) {
            setupHook_(target);
        }
    });

    connection->setEventCallback([this](const EventDelivery &delivery) {
        if (!eventHook_) {
            return;
        }
        auto managed = lookup(delivery.device);
        if (managed != nullptr && managed->connection) {
            eventHook_(*managed->connection, delivery);
        }
    });

    connection->setStateCallback([this](const ClientConnection &target) {
        if (stateHook_) {
            stateHook_(target);
        }
    });

    connection->setErrorCallback([this](const ClientConnection &target, const ClientError &error) {
        if (errorHook_) {
            errorHook_(target, error);
        }
    });

    // Reconnect recreates the transport without recreating the connection
    // object, so diagnostics and backoff survive a drop (§56).
    const domain::DeviceUuid uuid = entry.uuid;
    connection->setReconnectHandler([this, uuid](ClientConnection &) { reconnectFor(uuid); });
}

void ConnectionManager::reconnectFor(const domain::DeviceUuid &uuid) {
    Managed *entry = lookup(uuid);
    if (entry == nullptr || !entry->connection) {
        return;
    }

    auto transport = factory_.create(entry->endpoint, *entry->connection);
    if (!transport) {
        // No transport available right now: stay in ReconnectWait and retry on
        // the next backoff step.
        return;
    }
    entry->connection->bindTransport(std::move(transport));
    entry->connection->reconnectNow();
}

ClientResult<std::shared_ptr<ClientConnection>> ConnectionManager::open(const domain::DeviceUuid &uuid,
                                                                       const Endpoint &endpoint) {
    if (endpoint.host.empty()) {
        return ClientResult<std::shared_ptr<ClientConnection>>::error(ErrorDomain::Application,
                                                                     ClientErrorCode::InvalidArgument,
                                                                     "connection endpoint has no host");
    }

    Managed *entry = lookup(uuid);
    if (entry != nullptr) {
        if (entry->connection && entry->connection->isActive()) {
            return ClientResult<std::shared_ptr<ClientConnection>>::ok(entry->connection); // idempotent open
        }
        entry->endpoint = endpoint;
        entry->connection->noteEndpoint(endpoint);
        reconnectFor(uuid);
        return ClientResult<std::shared_ptr<ClientConnection>>::ok(entry->connection);
    }

    const std::uint16_t address = allocateAddress();
    if (address == 0U) {
        return ClientResult<std::shared_ptr<ClientConnection>>::error(
            ErrorDomain::Application, ClientErrorCode::InvalidState, "no client logical address available");
    }

    ConnectionOptions connectionOptions;
    connectionOptions.deviceUuid = uuid;
    connectionOptions.clientInstanceUuid = options_.clientInstanceUuid;
    connectionOptions.clientLogicalAddress = address;
    connectionOptions.desiredRole = options_.desiredRole;
    connectionOptions.timeouts = options_.timeouts;
    connectionOptions.autoReconnect = options_.autoReconnect;
    connectionOptions.maxOutstandingRequests = options_.maxOutstandingRequests;
    connectionOptions.maxTxQueueFrames = options_.maxTxQueueFrames;
    connectionOptions.reconnectJitter = options_.reconnectJitter;

    auto managed = std::make_unique<Managed>();
    managed->uuid = uuid;
    managed->endpoint = endpoint;
    managed->clientLogicalAddress = address;
    managed->connection = std::make_shared<ClientConnection>(connectionOptions, clock_, executor_);
    managed->connection->noteEndpoint(endpoint);

    Managed *stable = managed.get();
    managed_.push_back(std::move(managed));
    installCallbacks(*stable);

    auto transport = factory_.create(endpoint, *stable->connection);
    if (!transport) {
        managed_.erase(managed_.begin() + static_cast<std::ptrdiff_t>(managed_.size() - 1));
        return ClientResult<std::shared_ptr<ClientConnection>>::error(ErrorDomain::Network, ClientErrorCode::ConnectFailed,
                                                                     "transport factory refused to create a transport");
    }
    stable->connection->bindTransport(std::move(transport));
    stable->connection->start();

    return ClientResult<std::shared_ptr<ClientConnection>>::ok(stable->connection);
}

void ConnectionManager::close(const domain::DeviceUuid &uuid) {
    for (std::size_t index = 0; index < managed_.size(); ++index) {
        if (managed_[index]->uuid != uuid) {
            continue;
        }
        managed_[index]->connection->close();
        managed_.erase(managed_.begin() + static_cast<std::ptrdiff_t>(index));
        return;
    }
}

void ConnectionManager::closeAll() {
    for (auto &holder : managed_) {
        holder->connection->close();
    }
    managed_.clear();
}

void ConnectionManager::tick(std::uint64_t nowUs) {
    for (auto &holder : managed_) {
        holder->connection->tick(nowUs);
    }
}

std::shared_ptr<ClientConnection> ConnectionManager::find(const domain::DeviceUuid &uuid) const {
    for (const auto &holder : managed_) {
        if (holder->uuid == uuid) {
            return holder->connection;
        }
    }
    return {};
}

std::shared_ptr<ClientConnection> ConnectionManager::findByAddress(std::uint16_t clientLogicalAddress) const {
    for (const auto &holder : managed_) {
        if (holder->clientLogicalAddress == clientLogicalAddress) {
            return holder->connection;
        }
    }
    return {};
}

std::size_t ConnectionManager::activeCount() const {
    std::size_t active = 0;
    for (const auto &holder : managed_) {
        if (holder->connection->isActive()) {
            ++active;
        }
    }
    return active;
}

std::vector<std::shared_ptr<ClientConnection>> ConnectionManager::connections() const {
    std::vector<std::shared_ptr<ClientConnection>> out;
    out.reserve(managed_.size());
    for (const auto &holder : managed_) {
        out.push_back(holder->connection);
    }
    return out;
}

} // namespace uwb::client
