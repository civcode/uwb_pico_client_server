#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "uwb/client/client_clock.hpp"
#include "uwb/client/client_error.hpp"
#include "uwb/client/client_service.hpp"
#include "uwb/client/client_transport.hpp"
#include "uwb/client/client_types.hpp"
#include "uwb/client/transaction.hpp"
#include "uwb/protocol/frame_parser.hpp"

namespace uwb::client {

struct ConnectionOptions {
    // Expected device identity. A zero UUID accepts whatever the device reports
    // during activation (used when a user connects by endpoint only).
    domain::DeviceUuid deviceUuid;
    domain::DeviceUuid clientInstanceUuid;

    std::uint16_t clientLogicalAddress = static_cast<std::uint16_t>(uwb::protocol::kClientLogicalAddressMin + 1);
    uwb::protocol::ConnectionRole desiredRole = uwb::protocol::ConnectionRole::Control;

    ClientTimeouts timeouts;
    bool autoReconnect = true;
    std::uint32_t maxOutstandingRequests = 16;
    std::uint32_t maxTxQueueFrames = 32;

    // Random jitter added to the reconnect backoff by the networking layer.
    // The core default is zero so that unit tests observe exact delays.
    std::function<std::uint32_t(std::uint32_t)> reconnectJitter;
};

struct ConnectionDiagnostics {
    std::uint64_t framesSent = 0;
    std::uint64_t framesReceived = 0;
    std::uint64_t requestsSent = 0;
    std::uint64_t responsesMatched = 0;
    std::uint64_t eventNotifications = 0;
    std::uint64_t acksReceived = 0;
    std::uint64_t transportNacks = 0;
    std::uint64_t negativeResponses = 0;
    std::uint64_t requestTimeouts = 0;
    std::uint64_t lateResponses = 0;
    std::uint64_t unmatchedResponses = 0;
    std::uint64_t protocolErrors = 0;
    std::uint64_t txQueueDrops = 0;
    std::uint32_t reconnectAttempts = 0;
    std::uint64_t lastActivityUs = 0;
};

// One Pico connection (specification §56).
//
// Threading contract: every method of ClientConnection runs on the single
// network I/O thread. Application code reaches it through
// ApplicationController, which posts commands to the executor.
class ClientConnection {
public:
    using SetupCallback = std::function<void(ClientConnection &)>;
    using ReconnectCallback = std::function<void(ClientConnection &)>;
    using StateCallback = std::function<void(const ClientConnection &)>;
    using ErrorCallback = std::function<void(const ClientConnection &, const ClientError &)>;

    ClientConnection(const ConnectionOptions &options, IClock &clock, ITaskExecutor &executor);

    // ---- wiring, performed on the network I/O thread -----------------------
    void bindTransport(std::unique_ptr<IClientTransport> transport);
    // Establish a connection and restart the reconnect backoff.
    void start();
    // Reconnect after a scheduled backoff: keeps the backoff progression (§73).
    void reconnectNow();
    void close(); // manual disconnect: no automatic reconnect (§73)
    void setEventCallback(EventCallback callback);
    void setSetupHandler(SetupCallback handler);
    void setReconnectHandler(ReconnectCallback handler);
    void setStateCallback(StateCallback handler);
    void setErrorCallback(ErrorCallback handler);
    void setDesiredRole(uwb::protocol::ConnectionRole role) noexcept { options_.desiredRole = role; }

    // ---- transport adapter entry points (network I/O thread) --------------
    void onConnected();
    void onBytes(uwb::protocol::ConstBytes chunk);
    void onClosed(TransportStatus status, const std::string &message);

    // ---- periodic supervision (network I/O thread) ------------------------
    void tick(std::uint64_t nowUs);

    // ---- requests (network I/O thread) ------------------------------------
    [[nodiscard]] ClientResult<RequestId> sendRequest(const ServiceRequest &request, ResponseCallback completion);
    void cancelRequest(RequestId transactionId);

    // ---- observable state -------------------------------------------------
    [[nodiscard]] ConnectionState state() const noexcept { return state_; }
    [[nodiscard]] const domain::DeviceUuid &deviceUuid() const noexcept { return deviceUuid_; }
    [[nodiscard]] std::uint16_t clientLogicalAddress() const noexcept { return options_.clientLogicalAddress; }
    [[nodiscard]] std::uint16_t serverLogicalAddress() const noexcept { return serverLogicalAddress_; }
    [[nodiscard]] uwb::protocol::ConnectionRole role() const noexcept { return role_; }
    [[nodiscard]] uwb::protocol::SessionId session() const noexcept { return session_; }
    [[nodiscard]] std::uint32_t serverMaxPayload() const noexcept { return serverMaxPayload_; }
    [[nodiscard]] uwb::protocol::CapabilityMask effectiveCapabilities() const noexcept { return effectiveMask_; }
    [[nodiscard]] const std::string &endpointHost() const noexcept { return endpoint_.host; }
    [[nodiscard]] std::uint16_t endpointPort() const noexcept { return endpoint_.port; }
    [[nodiscard]] const ConnectionOptions &options() const noexcept { return options_; }
    [[nodiscard]] const std::vector<StreamSubscription> &subscriptions() const noexcept { return subscriptions_; }
    [[nodiscard]] const ConnectionDiagnostics &diagnostics() const noexcept { return diagnostics_; }
    [[nodiscard]] const ClientError &lastError() const noexcept { return lastError_; }
    [[nodiscard]] bool isActive() const noexcept {
        return state_ == ConnectionState::Activated || state_ == ConnectionState::Ready;
    }
    [[nodiscard]] bool isBusy() const noexcept { return pending_.size() > 0; }
    [[nodiscard]] std::size_t pendingCount() const noexcept { return pending_.size(); }
    [[nodiscard]] std::uint64_t reconnectDelayUs() const noexcept { return reconnectDelayUs_; }

    // Session state is learned from the 0x10 response by the controller.
    void noteSession(uwb::protocol::SessionId session) noexcept { session_ = session; }
    void markReady();
    void noteEndpoint(const Endpoint &endpoint) { endpoint_ = endpoint; }

    // Stream bookkeeping updated by the controller after a subscribe response.
    void addSubscription(const StreamSubscription &subscription);
    void removeSubscription(std::uint16_t streamId);
    void clearSubscriptions();

private:
    void handleFrame(const uwb::protocol::Frame &frame);
    void handleApplicationMessage(const uwb::protocol::Frame &frame);
    void handleEventNotification(const uwb::protocol::Frame &frame);
    void handleActivationFrame(const uwb::protocol::Frame &frame);
    void handleAliveFrame(const uwb::protocol::Frame &frame);
    void handleApplicationAck(const uwb::protocol::Frame &frame);
    void handleApplicationNack(const uwb::protocol::Frame &frame);

    void beginConnect();
    void startActivation();
    void enqueueFrame(uwb::protocol::ByteBuffer frame, bool front);
    void flushTxQueue();
    void scheduleReconnect(std::uint64_t nowUs);
    void superviseAliveCheck(std::uint64_t nowUs);
    void superviseClientPresent(std::uint64_t nowUs);
    void expirePending(std::uint64_t nowUs);
    void completeTransaction(RequestId transactionId, ClientResult<ServiceResponse> result);
    void notifyError(const ClientError &error);
    void setState(ConnectionState state);
    [[nodiscard]] std::uint32_t requestTimeoutMs(std::uint32_t requestedMs) const noexcept;
    void updateSubscriptionFromStatus(const uwb::protocol::StreamStatusEvent &status);

    ConnectionOptions options_;
    IClock &clock_;
    ITaskExecutor &executor_;

    std::unique_ptr<IClientTransport> transport_;
    uwb::protocol::FrameParser parser_;
    std::deque<uwb::protocol::ByteBuffer> txQueue_;
    PendingRequestTable pending_;
    TransactionIdAllocator transactions_;

    ConnectionState state_ = ConnectionState::Disconnected;
    domain::DeviceUuid deviceUuid_;
    Endpoint endpoint_;
    std::uint16_t serverLogicalAddress_ = uwb::protocol::kLogicalAddressInvalid;
    uwb::protocol::ConnectionRole role_ = uwb::protocol::ConnectionRole::None;
    uwb::protocol::SessionId session_ = uwb::protocol::SessionId::Default;
    std::uint32_t serverMaxPayload_ = uwb::protocol::kMaxProtocolPayload;
    uwb::protocol::CapabilityMask effectiveMask_ = 0;

    ClientError lastError_;
    ConnectionDiagnostics diagnostics_;

    std::vector<StreamSubscription> subscriptions_;

    EventCallback eventCallback_;
    SetupCallback setupHandler_;
    ReconnectCallback reconnectHandler_;
    StateCallback stateCallback_;
    ErrorCallback errorCallback_;
    std::uint64_t aliveNonceCounter_ = 0;

    bool manualClose_ = false;
    bool activationOutstanding_ = false;
    std::uint64_t reconnectAtUs_ = 0;
    std::uint64_t reconnectDelayUs_ = 0;
    std::uint32_t reconnectAttempt_ = 0;

    std::uint64_t aliveNonce_ = 0;
    std::uint64_t aliveSentAtUs_ = 0;
    std::uint64_t lastClientPresentUs_ = 0;
    std::uint64_t lastActivityUs_ = 0;
};

} // namespace uwb::client
