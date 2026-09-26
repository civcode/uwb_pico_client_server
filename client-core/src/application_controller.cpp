#include "uwb/client/application_controller.hpp"

#include <algorithm>
#include <utility>

#include "uwb/protocol/capabilities.hpp"
#include "uwb/protocol/complete_config.hpp"
#include "uwb/protocol/constants.hpp"
#include "uwb/protocol/did_records.hpp"
#include "uwb/protocol/events.hpp"
#include "uwb/protocol/payloads.hpp"
#include "uwb/protocol/routines.hpp"
#include "uwb/protocol/service_ids.hpp"
#include "uwb/protocol/services.hpp"

namespace uwb::client {

namespace {

using namespace uwb::protocol;

// TCP endpoint of a discovered device: the answer arrives from the UDP source
// endpoint, the port comes from the identity record (§17).
[[nodiscard]] Endpoint tcpEndpointFor(const Endpoint &source, const DeviceIdResponse &identity, std::uint16_t fallbackPort) {
    return Endpoint{source.host, identity.tcpPort != 0 ? identity.tcpPort : fallbackPort};
}

[[nodiscard]] std::string textOf(ConstBytes bytes) {
    std::string out;
    out.reserve(bytes.size());
    for (std::uint8_t byte : bytes) {
        out.push_back(static_cast<char>(byte));
    }
    return out;
}

} // namespace

ApplicationController::ApplicationController(const ControllerConfig &config, IClock &clock, ITaskExecutor &executor,
                                             IClientTransportFactory &transportFactory,
                                             std::unique_ptr<IDiscoveryTransport> discoveryTransport)
    : config_(config), clock_(clock), executor_(executor), registry_(config.discoveryStaleTimeoutMs),
      notifications_(config.notificationCapacity) {
    connections_ = std::make_unique<ConnectionManager>(config_.options, clock_, executor_, transportFactory);
    discovery_ = std::make_unique<DiscoveryClient>(config_.discovery, clock_, executor_, std::move(discoveryTransport));
}

ApplicationController::~ApplicationController() {
    stop();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void ApplicationController::start() {
    if (running_) {
        return;
    }
    running_ = true;

    discovery_->setDeviceSeenCallback([this](const Endpoint &source, const DeviceIdResponse &identity) {
        onDeviceSeen(source, identity);
    });
    discovery_->setErrorCallback([this](const ClientError &error) {
        ClientError copy = error;
        copy.domain = ErrorDomain::Network;
        {
            ClientNotification note;
            note.kind = NotificationKind::DeviceError;
            note.error = std::move(copy);
            publishNotification(std::move(note));
        }
    });

    connections_->setHooks([this](ClientConnection &connection) { onSetup(connection); },
                           [this](const ClientConnection &connection, const EventDelivery &delivery) {
                               onEvent(connection, delivery);
                           },
                           [this](const ClientConnection &connection) { onConnectionState(connection); },
                           [this](const ClientConnection &connection, const ClientError &error) {
                               onConnectionError(connection, error);
                           });
    connections_->setDeviceNameResolver([this](const domain::DeviceUuid &uuid) {
        const DeviceEntry *entry = registry_.find(uuid);
        return entry != nullptr ? entry->identity.deviceName : std::string{};
    });

    discovery_->start();
    refreshSnapshots();
}

void ApplicationController::stop() {
    if (!running_) {
        return;
    }
    running_ = false;
    if (connections_) {
        connections_->closeAll();
    }
    if (discovery_) {
        discovery_->stop();
    }
    notifications_.close();
}

void ApplicationController::tick(std::uint64_t nowUs) {
    if (!running_) {
        return;
    }
    if (connections_) {
        connections_->tick(nowUs);
    }
    if (discovery_) {
        discovery_->tick(nowUs);
    }
    expireProbes(nowUs);
    static_cast<void>(registry_.pruneStale(nowUs));
    refreshSnapshots();
}

// ---------------------------------------------------------------------------
// Discovery and registry
// ---------------------------------------------------------------------------

void ApplicationController::discoverNow() {
    if (discovery_) {
        discovery_->probeNow();
    }
}

void ApplicationController::onDeviceSeen(const Endpoint &source, const DeviceIdResponse &identity) {
    registry_.updateSeen(identity, source, clock_.nowUs());

    DeviceEntry *entry = registry_.find(identity.deviceUuid);
    if (entry == nullptr) {
        return;
    }
    entry->tcpEndpoint = tcpEndpointFor(source, identity, config_.discovery.port);

    // A pending endpoint probe resolves here.
    for (std::size_t index = 0; index < probes_.size(); ++index) {
        const Endpoint &probeEndpoint = probes_[index].endpoint;
        if (probeEndpoint.host != source.host) {
            continue;
        }
        // One host can answer for several devices (simulators on the loopback,
        // or a host running more than one Pico), so the port must match as well:
        // either the port the answer came from or the TCP port it advertises.
        if (probeEndpoint.port != source.port && probeEndpoint.port != identity.tcpPort) {
            continue;
        }
        PendingProbe probe = probes_[index];
        probes_.erase(probes_.begin() + static_cast<std::ptrdiff_t>(index));
        static_cast<void>(connections_->open(entry->uuid, entry->tcpEndpoint));
        registry_.attachConnection(entry->uuid, connections_->find(entry->uuid));
        {
            ClientNotification note;
            note.kind = NotificationKind::DeviceSeen;
            note.device = summarize(*entry);
            note.text = "connected to " + probe.endpoint.toString();
            publishNotification(std::move(note));
        }
        refreshSnapshots();
        return;
    }

    {
            ClientNotification note;
            note.kind = NotificationKind::DeviceSeen;
            note.device = summarize(*entry);
            publishNotification(std::move(note));
        }
    refreshSnapshots();
}

void ApplicationController::expireProbes(std::uint64_t nowUs) {
    for (std::size_t index = 0; index < probes_.size();) {
        if (nowUs < probes_[index].expiresAtUs) {
            ++index;
            continue;
        }
        const Endpoint endpoint = probes_[index].endpoint;
        probes_.erase(probes_.begin() + static_cast<std::ptrdiff_t>(index));
        ClientError error = makeError(ErrorDomain::Network, ClientErrorCode::Timeout,
                                      "no Device Id Response from " + endpoint.toString());
        {
            ClientNotification note;
            note.kind = NotificationKind::DeviceError;
            note.error = std::move(error);
            publishNotification(std::move(note));
        }
    }
}

std::vector<DeviceSummary> ApplicationController::listDevices() const {
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    return summaries_;
}

std::optional<DeviceSummary> ApplicationController::deviceSummary(const domain::DeviceUuid &uuid) const {
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    for (const DeviceSummary &summary : summaries_) {
        if (summary.uuid == uuid) {
            return summary;
        }
    }
    return std::nullopt;
}

std::optional<domain::DeviceUuid> ApplicationController::resolveDevice(std::string_view nameOrUuid) const {
    auto byUuid = domain::uuidFromText(nameOrUuid);
    if (byUuid.has_value() && registry_.contains(*byUuid)) {
        return *byUuid;
    }

    std::string name(nameOrUuid);
    if (const DeviceEntry *entry = registry_.byName(name); entry != nullptr) {
        return entry->uuid;
    }

    // Accept an unambiguous hex prefix of a UUID, which is what the CLI types.
    std::optional<domain::DeviceUuid> match;
    for (const DeviceEntry *entry : registry_.all()) {
        const std::string text = domain::uuidToString(entry->uuid);
        if (text.size() >= name.size() && !name.empty() && text.compare(0, name.size(), name) == 0) {
            if (match.has_value()) {
                return std::nullopt; // ambiguous
            }
            match = entry->uuid;
        }
    }
    return match;
}

std::size_t ApplicationController::discoveryRequestsSent() const { return discovery_->requestsSent(); }

// ---------------------------------------------------------------------------
// Connection commands
// ---------------------------------------------------------------------------

ClientResult<domain::DeviceUuid> ApplicationController::connectDevice(const domain::DeviceUuid &uuid) {
    const DeviceEntry *entry = registry_.find(uuid);
    if (entry == nullptr) {
        return ClientResult<domain::DeviceUuid>::error(ErrorDomain::Application, ClientErrorCode::DeviceNotFound,
                                                       "device is not in the discovery registry");
    }
    auto opened = connections_->open(uuid, entry->tcpEndpoint);
    if (!opened.ok()) {
        return ClientResult<domain::DeviceUuid>::error(opened.error());
    }
    registry_.attachConnection(uuid, opened.value());
    refreshSnapshots();
    return ClientResult<domain::DeviceUuid>::ok(uuid);
}

ClientResult<std::string> ApplicationController::connectEndpoint(const Endpoint &endpoint, std::string_view label) {
    if (endpoint.host.empty()) {
        return ClientResult<std::string>::error(ErrorDomain::Application, ClientErrorCode::InvalidArgument,
                                                "endpoint has no host");
    }

    for (const DeviceEntry *entry : registry_.all()) {
        if (entry->tcpEndpoint == endpoint || entry->discoveryEndpoint == endpoint) {
            auto opened = connections_->open(entry->uuid, entry->tcpEndpoint);
            if (!opened.ok()) {
                return ClientResult<std::string>::error(opened.error());
            }
            registry_.attachConnection(entry->uuid, opened.value());
            refreshSnapshots();
            return ClientResult<std::string>::ok("connecting to " + endpoint.toString());
        }
    }

    PendingProbe probe;
    probe.endpoint = endpoint;
    probe.label = std::string(label);
    probe.expiresAtUs = clock_.nowUs() + static_cast<std::uint64_t>(config_.options.timeouts.discoveryWindowMs) * 1000ULL;
    probes_.push_back(std::move(probe));

    // Probe the requested address directly; the periodic targets may not cover it.
    if (discovery_) {
        discovery_->probeTarget(endpoint);
    }
    discoverNow();
    return ClientResult<std::string>::ok("probing " + endpoint.toString());
}

void ApplicationController::disconnectDevice(const domain::DeviceUuid &uuid) {
    connections_->close(uuid);
    registry_.detachConnection(uuid);
    registry_.setConnectionState(uuid, ConnectionState::Disconnected);
    desiredStreams_.erase(uuid);
    refreshSnapshots();
}

void ApplicationController::disconnectAll() {
    connections_->closeAll();
    for (const DeviceEntry *entry : registry_.all()) {
        registry_.detachConnection(entry->uuid);
    }
    desiredStreams_.clear();
    refreshSnapshots();
}

void ApplicationController::onSetup(ClientConnection &connection) {
    const domain::DeviceUuid uuid = connection.deviceUuid();
    registry_.attachConnection(uuid, connections_->find(uuid));

    // Refresh the authoritative connection status after (re)activation (§58).
    ServiceRequest request;
    request.sid = static_cast<std::uint8_t>(ServiceId::ReadDataByIdentifier);
    request.pdu = encodeReadDidRequest(ReadDidRequest{static_cast<std::uint16_t>(Did::ConnectionStatus)});
    sendServiceRequest(uuid, request, [this, uuid](ClientResult<ServiceResponse> result) {
        if (result.ok()) {
            cacheReadResponse(uuid, result.value());
        }
    });

    resubscribe(connection);
}

void ApplicationController::onConnectionState(const ClientConnection &connection) {
    registry_.setConnectionState(connection.deviceUuid(), connection.state());
    if (connection.state() == ConnectionState::Disconnected) {
        registry_.detachConnection(connection.deviceUuid());
    }
    refreshSnapshots();
    auto summary = deviceSummary(connection.deviceUuid());
    if (summary.has_value()) {
        {
            ClientNotification note;
            note.kind = NotificationKind::DeviceStateChanged;
            note.device = summary;
            publishNotification(std::move(note));
        }
    }
}

void ApplicationController::onConnectionError(const ClientConnection &connection, const ClientError &error) {
    ClientError copy = error;
    copy.device = connection.deviceUuid();
    {
            ClientNotification note;
            note.kind = NotificationKind::DeviceError;
            note.device = deviceSummary(connection.deviceUuid());
            note.error = std::move(copy);
            publishNotification(std::move(note));
        }
}

// ---------------------------------------------------------------------------
// Service commands (§25.2: every command is one correlated transaction)
// ---------------------------------------------------------------------------

void ApplicationController::sendServiceRequest(const domain::DeviceUuid &uuid, const ServiceRequest &request,
                                              DidCallback callback) {
    auto connection = connections_->find(uuid);
    if (!connection || !connection->isActive()) {
        if (callback) {
            ClientError error = makeError(ErrorDomain::Network, ClientErrorCode::NotConnected, "device is not connected");
            error.device = uuid;
            callback(ClientResult<ServiceResponse>::error(std::move(error)));
        }
        return;
    }

    auto sent = connection->sendRequest(request, [callback = std::move(callback)](ClientResult<ServiceResponse> result) {
        if (callback) {
            callback(std::move(result));
        }
    });
    if (!sent.ok() && callback) {
        callback(ClientResult<ServiceResponse>::error(sent.error()));
    }
}

void ApplicationController::requestSession(const domain::DeviceUuid &uuid, SessionId session, CommandCallback callback) {
    callback = trackedCommand(uuid, std::move(callback));
    ServiceRequest request;
    request.sid = static_cast<std::uint8_t>(ServiceId::SessionControl);
    request.pdu = encodeSessionControlRequest(SessionControlRequest{static_cast<std::uint8_t>(session)});

    sendServiceRequest(uuid, request, [this, uuid, session, callback = std::move(callback)](ClientResult<ServiceResponse> result) {
        if (!result.ok()) {
            if (callback) {
                callback(ClientResult<std::string>::error(result.error()));
            }
            return;
        }
        auto decoded = decodeSessionControlResponse(result.value().pduBytes());
        if (!decoded.ok()) {
            if (callback) {
                callback(ClientResult<std::string>::error(ErrorDomain::Protocol, ClientErrorCode::DecodeError,
                                                         "malformed Session Control response"));
            }
            return;
        }
        if (auto held = connections_->find(uuid); held) {
            held->noteSession(static_cast<SessionId>(decoded.value().activeSession));
            held->markReady();
        }
        refreshSnapshots();
        if (callback) {
            callback(ClientResult<std::string>::ok("session " + sessionName(static_cast<SessionId>(decoded.value().activeSession))));
        }
    });
    static_cast<void>(session);
}

void ApplicationController::readDid(const domain::DeviceUuid &uuid, Did did, DidCallback callback) {
    ServiceRequest request;
    request.sid = static_cast<std::uint8_t>(ServiceId::ReadDataByIdentifier);
    request.pdu = encodeReadDidRequest(ReadDidRequest{static_cast<std::uint16_t>(did)});

    sendServiceRequest(uuid, request, [this, uuid, callback = std::move(callback)](ClientResult<ServiceResponse> result) {
        if (result.ok()) {
            cacheReadResponse(uuid, result.value());
            refreshSnapshots();
        }
        if (callback) {
            callback(std::move(result));
        }
    });
}

void ApplicationController::writeDid(const domain::DeviceUuid &uuid, Did did, const ByteBuffer &data,
                                    CommandCallback callback) {
    callback = trackedCommand(uuid, std::move(callback));
    ServiceRequest request;
    request.sid = static_cast<std::uint8_t>(ServiceId::WriteDataByIdentifier);
    request.pdu = encodeWriteDidRequest(WriteDidRequest{static_cast<std::uint16_t>(did), data});

    sendServiceRequest(uuid, request, [this, uuid, callback = std::move(callback)](ClientResult<ServiceResponse> result) {
        if (result.ok()) {
            refreshSnapshots();
        }
        if (callback) {
            callback(result.ok() ? ClientResult<std::string>::ok("written")
                                 : ClientResult<std::string>::error(result.error()));
        }
    });
}

void ApplicationController::readConfiguration(const domain::DeviceUuid &uuid, DidCallback callback) {
    readDid(uuid, Did::UwbCompleteConfiguration, std::move(callback));
}

void ApplicationController::writeConfiguration(const domain::DeviceUuid &uuid, const CompleteUwbConfig &config,
                                              CommandCallback callback) {
    callback = trackedCommand(uuid, std::move(callback));
    auto valid = validateCompleteUwbConfig(config);
    if (!valid.ok()) {
        if (callback) {
            callback(ClientResult<std::string>::error(
                ErrorDomain::Configuration, ClientErrorCode::InvalidArgument,
                std::string("invalid complete configuration: ") + protocolErrorName(valid.code())));
        }
        return;
    }

    auto connection = connections_->find(uuid);
    if (!connection || !connection->isActive()) {
        if (callback) {
            callback(ClientResult<std::string>::error(ErrorDomain::Network, ClientErrorCode::NotConnected,
                                                      "device is not connected"));
        }
        return;
    }

    // Large configuration records use a longer deadline (§21.2 p2*).
    ServiceRequest request;
    request.sid = static_cast<std::uint8_t>(ServiceId::WriteDataByIdentifier);
    request.timeoutMs = config_.options.timeouts.longRoutineTimeoutMs;
    request.pdu = encodeWriteDidRequest(WriteDidRequest{static_cast<std::uint16_t>(Did::UwbCompleteConfiguration),
                                                        encodeCompleteUwbConfig(config)});

    sendServiceRequest(uuid, request, [this, uuid, callback = std::move(callback)](ClientResult<ServiceResponse> result) {
        if (result.ok()) {
            refreshSnapshots();
        }
        if (callback) {
            callback(result.ok() ? ClientResult<std::string>::ok("configuration written")
                                 : ClientResult<std::string>::error(result.error()));
        }
    });
}

void ApplicationController::saveConfiguration(const domain::DeviceUuid &uuid, CommandCallback callback) {
    callback = trackedCommand(uuid, std::move(callback));
    startRoutine(uuid, RoutineId::SaveUwbConfiguration, {}, std::move(callback));
}

// ---------------------------------------------------------------------------
// Measurement streams (§31, §35)
// ---------------------------------------------------------------------------

void ApplicationController::startMeasurementStream(const domain::DeviceUuid &uuid, StreamMode mode,
                                                  CommandCallback callback) {
    callback = trackedCommand(uuid, std::move(callback));
    ServiceRequest request;
    request.sid = static_cast<std::uint8_t>(ServiceId::EventControl);
    EventSubscribeRequest subscribe;
    subscribe.eventId = static_cast<std::uint16_t>(EventId::UwbMeasurement);
    subscribe.mode = static_cast<std::uint8_t>(mode);
    subscribe.flags = 0;
    subscribe.requestedPeriodMs = 0; // source-native rate
    request.pdu = encodeEventSubscribeRequest(subscribe);

    sendServiceRequest(uuid, request, [this, uuid, mode, callback = std::move(callback)](ClientResult<ServiceResponse> result) {
        if (!result.ok()) {
            if (callback) {
                callback(ClientResult<std::string>::error(result.error()));
            }
            return;
        }
        auto decoded = decodeEventSubscribeResponse(result.value().pduBytes());
        if (!decoded.ok()) {
            if (callback) {
                callback(ClientResult<std::string>::error(ErrorDomain::Protocol, ClientErrorCode::DecodeError,
                                                         "malformed Event Subscribe response"));
            }
            return;
        }

        StreamSubscription subscription;
        subscription.streamId = decoded.value().streamId;
        subscription.eventId = decoded.value().eventId;
        subscription.mode = decoded.value().acceptedMode;
        subscription.flags = decoded.value().flags;
        subscription.periodMs = decoded.value().acceptedPeriodMs;
        subscription.queueCapacity = decoded.value().queueCapacity;
        subscription.state = StreamState::Active;

        desiredStreams_[uuid] = subscription;
        if (auto held = connections_->find(uuid); held) {
            held->addSubscription(subscription);
            refreshSnapshots(); // subscriptions are part of the device view (§59)
        }
        {
            ClientNotification note;
            note.kind = NotificationKind::StreamStateChanged;
            note.device = deviceSummary(uuid);
            note.stream = subscription;
            publishNotification(std::move(note));
        }
        if (callback) {
            callback(ClientResult<std::string>::ok("stream " + std::to_string(subscription.streamId) + " " +
                                                   std::string(mode == StreamMode::Live ? "live" : "recording")));
        }
    });
}

void ApplicationController::stopMeasurementStream(const domain::DeviceUuid &uuid, CommandCallback callback) {
    callback = trackedCommand(uuid, std::move(callback));
    auto connection = connections_->find(uuid);
    if (!connection || !connection->isActive()) {
        if (callback) {
            callback(ClientResult<std::string>::error(ErrorDomain::Network, ClientErrorCode::NotConnected,
                                                      "device is not connected"));
        }
        return;
    }

    std::uint16_t streamId = 0;
    for (const StreamSubscription &subscription : connection->subscriptions()) {
        if (subscription.eventId == static_cast<std::uint16_t>(EventId::UwbMeasurement)) {
            streamId = subscription.streamId;
            break;
        }
    }
    if (streamId == 0) {
        desiredStreams_.erase(uuid);
        if (callback) {
            callback(ClientResult<std::string>::error(ErrorDomain::Stream, ClientErrorCode::InvalidState,
                                                      "no measurement stream on this connection"));
        }
        return;
    }

    ServiceRequest request;
    request.sid = static_cast<std::uint8_t>(ServiceId::EventControl);
    request.pdu = encodeEventUnsubscribeRequest(EventUnsubscribeRequest{streamId});

    sendServiceRequest(uuid, request, [this, uuid, streamId, callback = std::move(callback)](ClientResult<ServiceResponse> result) {
        desiredStreams_.erase(uuid);
        if (auto held = connections_->find(uuid); held) {
            held->removeSubscription(streamId);
            refreshSnapshots();
        }
        if (callback) {
            callback(result.ok() ? ClientResult<std::string>::ok("stream " + std::to_string(streamId) + " stopped")
                                 : ClientResult<std::string>::error(result.error()));
        }
    });
}

void ApplicationController::resubscribe(ClientConnection &connection) {
    const domain::DeviceUuid uuid = connection.deviceUuid();
    auto desired = desiredStreams_.find(uuid);
    if (desired == desiredStreams_.end()) {
        return;
    }

    ServiceRequest request;
    request.sid = static_cast<std::uint8_t>(ServiceId::EventControl);
    EventSubscribeRequest subscribe;
    subscribe.eventId = desired->second.eventId;
    subscribe.mode = desired->second.mode;
    subscribe.flags = desired->second.flags;
    subscribe.requestedPeriodMs = desired->second.periodMs;
    request.pdu = encodeEventSubscribeRequest(subscribe);

    sendServiceRequest(uuid, request, [this, uuid](ClientResult<ServiceResponse> result) {
        if (!result.ok()) {
            desiredStreams_.erase(uuid);
            return;
        }
        auto decoded = decodeEventSubscribeResponse(result.value().pduBytes());
        if (!decoded.ok()) {
            desiredStreams_.erase(uuid);
            return;
        }
        StreamSubscription subscription;
        subscription.streamId = decoded.value().streamId;
        subscription.eventId = decoded.value().eventId;
        subscription.mode = decoded.value().acceptedMode;
        subscription.flags = decoded.value().flags;
        subscription.periodMs = decoded.value().acceptedPeriodMs;
        subscription.queueCapacity = decoded.value().queueCapacity;
        desiredStreams_[uuid] = subscription;
        if (auto held = connections_->find(uuid); held) {
            held->addSubscription(subscription);
            refreshSnapshots(); // subscriptions are part of the device view (§59)
        }
    });
}

// ---------------------------------------------------------------------------
// Routines, AT, reset, security
// ---------------------------------------------------------------------------

void ApplicationController::startRoutine(const domain::DeviceUuid &uuid, RoutineId routine, const ByteBuffer &options,
                                        CommandCallback callback) {
    callback = trackedCommand(uuid, std::move(callback));
    ServiceRequest request;
    request.sid = static_cast<std::uint8_t>(ServiceId::RoutineControl);
    request.timeoutMs = config_.options.timeouts.longRoutineTimeoutMs;
    RoutineControlRequest control;
    control.controlType = static_cast<std::uint8_t>(RoutineControlType::Start);
    control.routineId = static_cast<std::uint16_t>(routine);
    control.optionRecord = options;
    request.pdu = encodeRoutineControlRequest(control);

    sendServiceRequest(uuid, request, [callback = std::move(callback), routine](ClientResult<ServiceResponse> result) {
        if (!callback) {
            return;
        }
        if (!result.ok()) {
            callback(ClientResult<std::string>::error(result.error()));
            return;
        }
        auto decoded = decodeRoutineControlResponse(result.value().pduBytes());
        if (!decoded.ok()) {
            callback(ClientResult<std::string>::error(ErrorDomain::Protocol, ClientErrorCode::DecodeError,
                                                      "malformed Routine Control response"));
            return;
        }
        const std::string text = std::string(routineName(routine)) + " started, state 0x" +
                                 std::to_string(static_cast<unsigned>(decoded.value().routineState));
        callback(ClientResult<std::string>::ok(std::move(text)));
    });
}

void ApplicationController::stopRoutine(const domain::DeviceUuid &uuid, CommandCallback callback) {
    callback = trackedCommand(uuid, std::move(callback));
    auto connection = connections_->find(uuid);
    if (!connection || !connection->isActive()) {
        if (callback) {
            callback(ClientResult<std::string>::error(ErrorDomain::Network, ClientErrorCode::NotConnected,
                                                      "device is not connected"));
        }
        return;
    }

    ServiceRequest request;
    request.sid = static_cast<std::uint8_t>(ServiceId::RoutineControl);
    RoutineControlRequest control;
    control.controlType = static_cast<std::uint8_t>(RoutineControlType::Stop);
    control.routineId = static_cast<std::uint16_t>(RoutineId::UwbMeasurementAcquisition);
    request.pdu = encodeRoutineControlRequest(control);

    sendServiceRequest(uuid, request, [callback = std::move(callback)](ClientResult<ServiceResponse> result) {
        if (!callback) {
            return;
        }
        callback(result.ok() ? ClientResult<std::string>::ok("acquisition stopped")
                             : ClientResult<std::string>::error(result.error()));
    });
}

void ApplicationController::executeAtCommand(const domain::DeviceUuid &uuid, std::string_view command,
                                            CommandCallback callback) {
    callback = trackedCommand(uuid, std::move(callback));
    if (command.empty() || command.size() > kMaxAtCommandLength) {
        if (callback) {
            callback(ClientResult<std::string>::error(ErrorDomain::Application, ClientErrorCode::InvalidArgument,
                                                      "AT command length must be 1..512"));
        }
        return;
    }

    ServiceRequest request;
    request.sid = static_cast<std::uint8_t>(ServiceId::ExecuteAtCommand);
    request.timeoutMs = config_.options.timeouts.longRoutineTimeoutMs;
    ExecuteAtRequest at;
    at.command.assign(command.begin(), command.end());
    request.pdu = encodeExecuteAtRequest(at);

    sendServiceRequest(uuid, request, [callback = std::move(callback)](ClientResult<ServiceResponse> result) {
        if (!callback) {
            return;
        }
        if (!result.ok()) {
            callback(ClientResult<std::string>::error(result.error()));
            return;
        }
        auto decoded = decodeExecuteAtResponse(result.value().pduBytes());
        if (!decoded.ok()) {
            callback(ClientResult<std::string>::error(ErrorDomain::Protocol, ClientErrorCode::DecodeError,
                                                      "malformed AT response"));
            return;
        }
        callback(ClientResult<std::string>::ok(textOf(decoded.value().rawResponse)));
    });
}

void ApplicationController::resetDevice(const domain::DeviceUuid &uuid, DeviceResetType type, CommandCallback callback) {
    callback = trackedCommand(uuid, std::move(callback));
    ServiceRequest request;
    request.sid = static_cast<std::uint8_t>(ServiceId::PicoDeviceReset);
    request.timeoutMs = config_.options.timeouts.longRoutineTimeoutMs;
    request.pdu = encodeDeviceResetRequest(DeviceResetRequest{static_cast<std::uint8_t>(type)});

    sendServiceRequest(uuid, request, [callback = std::move(callback), type](ClientResult<ServiceResponse> result) {
        if (!callback) {
            return;
        }
        callback(result.ok() ? ClientResult<std::string>::ok(std::string(type == DeviceResetType::Hard ? "hard" : "soft") +
                                                             " reset accepted")
                             : ClientResult<std::string>::error(result.error()));
    });
}

void ApplicationController::securityUnlock(const domain::DeviceUuid &uuid, const ByteBuffer &key,
                                          CommandCallback callback) {
    callback = trackedCommand(uuid, std::move(callback));
    if (key.size() != kSecurityKeySize) {
        if (callback) {
            callback(ClientResult<std::string>::error(ErrorDomain::Application, ClientErrorCode::InvalidArgument,
                                                      "security key must be 4 bytes"));
        }
        return;
    }

    auto connection = connections_->find(uuid);
    if (!connection || !connection->isActive()) {
        if (callback) {
            callback(ClientResult<std::string>::error(ErrorDomain::Network, ClientErrorCode::NotConnected,
                                                      "device is not connected"));
        }
        return;
    }

    // Step 1: request the seed, then step 2: send the derived key (§25).
    ServiceRequest seedRequest;
    seedRequest.sid = static_cast<std::uint8_t>(ServiceId::SecurityAccess);
    seedRequest.pdu = encodeSecurityAccessRequest(
        SecurityAccessRequest{static_cast<std::uint8_t>(SecuritySubFunction::RequestSeed), 0});

    auto heldKey = std::make_shared<ByteBuffer>(key);
    sendServiceRequest(uuid, seedRequest,
                       [this, uuid, heldKey, callback = std::move(callback)](ClientResult<ServiceResponse> seedResult) {
                           if (!seedResult.ok()) {
                               if (callback) {
                                   callback(ClientResult<std::string>::error(seedResult.error()));
                               }
                               return;
                           }
                           auto seed = decodeSecurityAccessResponse(seedResult.value().pduBytes());
                           if (!seed.ok()) {
                               if (callback) {
                                   callback(ClientResult<std::string>::error(ErrorDomain::Protocol,
                                                                            ClientErrorCode::DecodeError,
                                                                            "malformed Security Access seed response"));
                               }
                               return;
                           }

                           // v1 key derivation is out of scope: the configured key
                           // is sent verbatim (docs/protocol_decisions.md).
                           std::uint32_t keyValue = 0;
                           for (std::uint8_t byte : *heldKey) {
                               keyValue = (keyValue << 8U) | static_cast<std::uint32_t>(byte);
                           }
                           static_cast<void>(seed.value().seed);

                           ServiceRequest keyRequest;
                           keyRequest.sid = static_cast<std::uint8_t>(ServiceId::SecurityAccess);
                           keyRequest.pdu = encodeSecurityAccessRequest(
                               SecurityAccessRequest{static_cast<std::uint8_t>(SecuritySubFunction::SendKey), keyValue});

                           sendServiceRequest(uuid, keyRequest,
                                              [callback = std::move(callback)](ClientResult<ServiceResponse> keyResult) {
                                                  if (!callback) {
                                                      return;
                                                  }
                                                  callback(keyResult.ok() ? ClientResult<std::string>::ok("security unlocked")
                                                                        : ClientResult<std::string>::error(keyResult.error()));
                                              });
                       });
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

void ApplicationController::onEvent(const ClientConnection &, const EventDelivery &delivery) {
    const domain::DeviceUuid uuid = delivery.device;
    const EventNotification &event = delivery.event;

    switch (static_cast<EventId>(event.eventId)) {
    case EventId::UwbMeasurement: {
        auto measurement = decodeMeasurementEvent(event.payloadBytes());
        if (!measurement.ok() || !config_.publishMeasurements) {
            return;
        }
        {
            ClientNotification note;
            note.kind = NotificationKind::Measurement;
            note.device = deviceSummary(uuid);
            note.measurement = domain::normalizeMeasurement(uuid, event, measurement.value(), clock_.nowUs());
            publishNotification(std::move(note));
        }
        return;
    }
    case EventId::UwbLocalPosition: {
        auto position = decodeLocalPositionEvent(event.payloadBytes());
        if (!position.ok()) {
            return;
        }
        {
            ClientNotification note;
            note.kind = NotificationKind::LocalPosition;
            note.device = deviceSummary(uuid);
            note.position = domain::normalizeLocalPosition(uuid, event, position.value(), clock_.nowUs());
            publishNotification(std::move(note));
        }
        return;
    }
    case EventId::StreamStatus: {
        auto status = decodeStreamStatusEvent(event.payloadBytes());
        if (!status.ok()) {
            return;
        }
        auto held = connections_->find(uuid);
        if (!held) {
            return;
        }
        for (const StreamSubscription &subscription : held->subscriptions()) {
            if (subscription.streamId != status.value().streamId) {
                continue;
            }
            StreamSubscription updated = subscription;
            updated.state = static_cast<StreamState>(status.value().state);
            updated.droppedCount = status.value().droppedCount;
            auto desired = desiredStreams_.find(uuid);
            if (desired != desiredStreams_.end()) {
                desired->second = updated;
            }
            {
            ClientNotification note;
            note.kind = NotificationKind::StreamStateChanged;
            note.device = deviceSummary(uuid);
            note.stream = updated;
            publishNotification(std::move(note));
        }
            return;
        }
        return;
    }
    default:
        {
            ClientNotification note;
            note.kind = NotificationKind::CommandFinished;
            note.device = deviceSummary(uuid);
            note.text = "event 0x" + std::to_string(event.eventId);
            publishNotification(std::move(note));
        }
        return;
    }
}

// ---------------------------------------------------------------------------
// Registry caching and snapshots
// ---------------------------------------------------------------------------

void ApplicationController::cacheReadResponse(const domain::DeviceUuid &uuid, const ServiceResponse &response) {
    if (response.negative || response.pdu.empty()) {
        return;
    }

    auto decoded = decodeServicePdu(response.pduBytes());
    if (!decoded.ok() || decoded.value().sid != static_cast<std::uint8_t>(ServiceId::ReadDataByIdentifier)) {
        return;
    }

    auto read = decodeReadDidResponse(response.pduBytes());
    if (!read.ok()) {
        return;
    }

    switch (static_cast<Did>(read.value().did)) {
    case Did::ConnectionStatus: {
        auto status = decodeConnectionStatusRecord(read.value().dataBytes());
        if (status.ok()) {
            registry_.setStatus(uuid, status.value());
        }
        break;
    }
    case Did::UwbCompleteConfiguration: {
        auto config = decodeCompleteUwbConfig(read.value().dataBytes());
        if (config.ok()) {
            registry_.setConfiguration(uuid, config.value());
            configCache_[uuid] = config.value();
        }
        break;
    }
    default:
        break;
    }
}

DeviceSummary ApplicationController::summarize(const DeviceEntry &entry) const {
    DeviceSummary summary;
    summary.uuid = entry.uuid;
    summary.name = entry.identity.deviceName;
    summary.endpoint = entry.tcpEndpoint.host.empty() ? entry.discoveryEndpoint : entry.tcpEndpoint;
    summary.logicalAddress = entry.identity.logicalAddress;
    summary.capabilities = entry.identity.capabilityMask;
    summary.activeObservers = entry.identity.activeObservers;
    summary.maxObservers = entry.identity.maxObservers;
    summary.controlOccupied = entry.identity.controlStatus == ControlStatus::Occupied;
    summary.lastSeenUs = entry.lastSeenUs;
    summary.state = entry.connectionState;

    if (entry.connection) {
        summary.state = entry.connection->state();
        summary.role = entry.connection->role();
        summary.session = entry.connection->session();
        summary.capabilities = entry.connection->effectiveCapabilities();
        summary.subscriptions = static_cast<std::uint32_t>(entry.connection->subscriptions().size());
        if (!entry.connection->lastError().message.empty()) {
            summary.hasError = true;
            summary.lastError = entry.connection->lastError().message;
        }
    }

    if (entry.status.has_value()) {
        summary.controlOccupied = entry.status->controlOccupied != 0;
        summary.activeObservers = entry.status->activeObservers;
        summary.maxObservers = entry.status->maxObservers;
    }

    return summary;
}

void ApplicationController::refreshSnapshots() {
    std::vector<DeviceSummary> fresh;
    fresh.reserve(registry_.size());
    for (const DeviceEntry *entry : registry_.all()) {
        fresh.push_back(summarize(*entry));
    }
    std::sort(fresh.begin(), fresh.end(), [](const DeviceSummary &lhs, const DeviceSummary &rhs) {
        if (lhs.name != rhs.name) {
            return lhs.name < rhs.name;
        }
        return lhs.uuid < rhs.uuid;
    });
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    summaries_ = std::move(fresh);
}

ApplicationController::CommandCallback ApplicationController::trackedCommand(const domain::DeviceUuid &uuid, CommandCallback callback) {
    return [this, uuid, callback = std::move(callback)](ClientResult<std::string> result) {
        pushCommandResult(uuid, result);
        if (callback) {
            callback(std::move(result));
        }
    };
}

void ApplicationController::pushCommandResult(const domain::DeviceUuid &uuid, ClientResult<std::string> result) {
    if (result.ok()) {
        {
            ClientNotification note;
            note.kind = NotificationKind::CommandFinished;
            note.device = deviceSummary(uuid);
            note.text = result.value();
            publishNotification(std::move(note));
        }
        return;
    }
    {
            ClientNotification note;
            note.kind = NotificationKind::DeviceError;
            note.device = deviceSummary(uuid);
            note.error = result.error();
            publishNotification(std::move(note));
        }
}

void ApplicationController::publishNotification(ClientNotification notification) {
    notifications_.push(std::move(notification));
}

ClientResult<ClientNotification> ApplicationController::waitNotification(std::chrono::milliseconds timeout) {
    ClientNotification notification;
    if (!notifications_.waitPop(notification, timeout)) {
        return ClientResult<ClientNotification>::error(ErrorDomain::Application, ClientErrorCode::Timeout,
                                                       "no notification within the timeout");
    }
    if (notifications_.closed()) {
        return ClientResult<ClientNotification>::error(ErrorDomain::Application, ClientErrorCode::InvalidState,
                                                       "notification queue is closed");
    }
    return ClientResult<ClientNotification>::ok(std::move(notification));
}

DiagnosticsSnapshot ApplicationController::diagnostics() const {
    DiagnosticsSnapshot snapshot;
    snapshot.discovered = registry_.size();
    snapshot.connections = connections_->count();
    snapshot.activeConnections = connections_->activeCount();
    snapshot.discoveryRequestsSent = discovery_->requestsSent();
    snapshot.discoveryResponses = discovery_->responsesReceived();
    snapshot.malformedDatagrams = discovery_->malformedDatagrams();
    snapshot.notificationsDropped = notifications_.dropped();

    for (const auto &connection : connections_->connections()) {
        const ConnectionDiagnostics &diagnostic = connection->diagnostics();
        snapshot.framesSent += diagnostic.framesSent;
        snapshot.framesReceived += diagnostic.framesReceived;
        snapshot.requestsSent += diagnostic.requestsSent;
        snapshot.responsesMatched += diagnostic.responsesMatched;
        snapshot.negativeResponses += diagnostic.negativeResponses;
        snapshot.requestTimeouts += diagnostic.requestTimeouts;
        snapshot.lateResponses += diagnostic.lateResponses;
        snapshot.unmatchedResponses += diagnostic.unmatchedResponses;
        snapshot.protocolErrors += diagnostic.protocolErrors;
        snapshot.txQueueDrops += diagnostic.txQueueDrops;
        snapshot.reconnectAttempts += diagnostic.reconnectAttempts;
    }
    return snapshot;
}

} // namespace uwb::client
