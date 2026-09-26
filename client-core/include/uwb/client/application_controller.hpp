#pragma once

#include <cstdint>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "uwb/client/client_clock.hpp"
#include "uwb/client/client_connection.hpp"
#include "uwb/client/client_error.hpp"
#include "uwb/client/client_service.hpp"
#include "uwb/client/client_transport.hpp"
#include "uwb/client/client_types.hpp"
#include "uwb/client/connection_manager.hpp"
#include "uwb/client/device_registry.hpp"
#include "uwb/client/discovery_client.hpp"
#include "uwb/client/notification_queue.hpp"
#include "uwb/domain/device_identity.hpp"
#include "uwb/domain/measurement.hpp"
#include "uwb/domain/uwb_configuration.hpp"

namespace uwb::client {

struct ControllerConfig {
    ClientOptions options;
    DiscoveryConfig discovery;
    std::uint32_t notificationCapacity = 512;
    std::uint32_t discoveryStaleTimeoutMs = 15000;
    bool publishMeasurements = true;
};

struct DiagnosticsSnapshot {
    std::size_t discovered = 0;
    std::size_t connections = 0;
    std::size_t activeConnections = 0;
    std::uint64_t discoveryRequestsSent = 0;
    std::uint64_t discoveryResponses = 0;
    std::uint64_t malformedDatagrams = 0;
    std::uint64_t notificationsDropped = 0;
    std::uint64_t framesSent = 0;
    std::uint64_t framesReceived = 0;
    std::uint64_t requestsSent = 0;
    std::uint64_t responsesMatched = 0;
    std::uint64_t negativeResponses = 0;
    std::uint64_t requestTimeouts = 0;
    std::uint64_t lateResponses = 0;
    std::uint64_t unmatchedResponses = 0;
    std::uint64_t protocolErrors = 0;
    std::uint64_t reconnectAttempts = 0;
    std::uint64_t txQueueDrops = 0;
};

// Application-facing façade: device registry, discovery, connections, streams,
// and the notification queue that decouples slow consumers from the I/O thread
// (specification §55, §56, §60; plan §25).
//
// Threading contract (§55, plan §25): the connections, the device registry, and
// the discovery socket are owned by the single I/O thread.  start(), tick(), and
// every command therefore run on that thread only.  Applications with their own
// main thread must marshal calls onto it - uwb::client_net::ClientSession is that
// façade.  The one member safe to touch from any thread is notifications().
class ApplicationController {
public:
    using DidCallback = std::function<void(ClientResult<ServiceResponse> result)>;
    using CommandCallback = std::function<void(ClientResult<std::string> result)>;

    ApplicationController(const ControllerConfig &config, IClock &clock, ITaskExecutor &executor,
                          IClientTransportFactory &transportFactory,
                          std::unique_ptr<IDiscoveryTransport> discoveryTransport);
    ~ApplicationController();

    ApplicationController(const ApplicationController &) = delete;
    ApplicationController &operator=(const ApplicationController &) = delete;

    void start();
    void stop();

    // Called from the I/O thread at the supervision interval.
    void tick(std::uint64_t nowUs);

    // --- discovery ---------------------------------------------------------
    void discoverNow();
    [[nodiscard]] std::vector<DeviceSummary> listDevices() const;
    [[nodiscard]] std::optional<DeviceSummary> deviceSummary(const domain::DeviceUuid &uuid) const;
    [[nodiscard]] std::optional<domain::DeviceUuid> resolveDevice(std::string_view nameOrUuid) const;
    [[nodiscard]] std::size_t discoveryRequestsSent() const;

    // Connect by UUID (the device must already be in the registry).
    ClientResult<domain::DeviceUuid> connectDevice(const domain::DeviceUuid &uuid);

    // Connect by endpoint: the device is probed once over UDP first, then the
    // TCP connection is opened when the identity answer arrives. The resulting
    // UUID surfaces as a DeviceStateChanged notification (§56).
    ClientResult<std::string> connectEndpoint(const Endpoint &endpoint, std::string_view label);
    void disconnectDevice(const domain::DeviceUuid &uuid);
    void disconnectAll();

    // --- session (§23) -----------------------------------------------------
    void requestSession(const domain::DeviceUuid &uuid, uwb::protocol::SessionId session, CommandCallback callback);

    // --- services (§25.2) --------------------------------------------------
    void readDid(const domain::DeviceUuid &uuid, uwb::protocol::Did did, DidCallback callback);
    void writeDid(const domain::DeviceUuid &uuid, uwb::protocol::Did did, const uwb::protocol::ByteBuffer &data,
                  CommandCallback callback);
    void readConfiguration(const domain::DeviceUuid &uuid, DidCallback callback);
    void writeConfiguration(const domain::DeviceUuid &uuid, const uwb::protocol::CompleteUwbConfig &config,
                            CommandCallback callback);
    void saveConfiguration(const domain::DeviceUuid &uuid, CommandCallback callback);

    // --- measurement streams (§31, §35) ------------------------------------
    void startMeasurementStream(const domain::DeviceUuid &uuid, uwb::protocol::StreamMode mode, CommandCallback callback);
    void stopMeasurementStream(const domain::DeviceUuid &uuid, CommandCallback callback);

    // --- routines and AT (§28, §30) ----------------------------------------
    void startRoutine(const domain::DeviceUuid &uuid, uwb::protocol::RoutineId routine,
                      const uwb::protocol::ByteBuffer &options, CommandCallback callback);
    void stopRoutine(const domain::DeviceUuid &uuid, CommandCallback callback);
    void executeAtCommand(const domain::DeviceUuid &uuid, std::string_view command, CommandCallback callback);
    void resetDevice(const domain::DeviceUuid &uuid, uwb::protocol::DeviceResetType type, CommandCallback callback);
    void securityUnlock(const domain::DeviceUuid &uuid, const uwb::protocol::ByteBuffer &key, CommandCallback callback);

    // --- notifications -----------------------------------------------------
    [[nodiscard]] ClientResult<ClientNotification> waitNotification(std::chrono::milliseconds timeout);
    [[nodiscard]] NotificationQueue &notifications() noexcept { return notifications_; }

    [[nodiscard]] DiagnosticsSnapshot diagnostics() const;

private:
    struct PendingProbe {
        Endpoint endpoint;
        std::string label;
        std::uint64_t expiresAtUs = 0;
    };

    void onDeviceSeen(const Endpoint &source, const uwb::protocol::DeviceIdResponse &identity);
    void onSetup(ClientConnection &connection);
    void onConnectionState(const ClientConnection &connection);
    void onConnectionError(const ClientConnection &connection, const ClientError &error);
    void onEvent(const ClientConnection &connection, const EventDelivery &delivery);

    void expireProbes(std::uint64_t nowUs);
    void sendServiceRequest(const domain::DeviceUuid &uuid, const ServiceRequest &request, DidCallback callback);
    void resubscribe(ClientConnection &connection);
    void restoreSubscriptions(ClientConnection &connection);
    void cacheReadResponse(const domain::DeviceUuid &uuid, const ServiceResponse &response);
    void refreshSnapshots();
    [[nodiscard]] DeviceSummary summarize(const DeviceEntry &entry) const;
    void publishNotification(ClientNotification notification);
    void pushCommandResult(const domain::DeviceUuid &uuid, ClientResult<std::string> result);
    // Wraps a command callback so every command completion also lands on the
    // notification queue as CommandFinished / DeviceError (specification §59, §69).
    [[nodiscard]] CommandCallback trackedCommand(const domain::DeviceUuid &uuid, CommandCallback callback);

    [[nodiscard]] ConnectionManager *connections() noexcept { return connections_.get(); }

    ControllerConfig config_;
    IClock &clock_;
    ITaskExecutor &executor_;
    std::unique_ptr<ConnectionManager> connections_;
    std::unique_ptr<DiscoveryClient> discovery_;
    DeviceRegistry registry_;
    NotificationQueue notifications_;

    mutable std::mutex snapshotMutex_;
    std::vector<DeviceSummary> summaries_;
    std::map<domain::DeviceUuid, StreamSubscription> desiredStreams_;
    std::vector<PendingProbe> probes_;
    std::map<domain::DeviceUuid, domain::UwbConfiguration> configCache_;

    std::size_t commandSequence_ = 0;
    bool running_ = false;
};
} // namespace uwb::client
