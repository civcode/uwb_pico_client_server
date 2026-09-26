#include "uwb/client/client_connection.hpp"

#include <utility>

#include "uwb/protocol/constants.hpp"
#include "uwb/protocol/errors.hpp"
#include "uwb/protocol/events.hpp"
#include "uwb/protocol/generic_header.hpp"
#include "uwb/protocol/payload_types.hpp"
#include "uwb/protocol/payloads.hpp"
#include "uwb/protocol/service_ids.hpp"
#include "uwb/protocol/services.hpp"

namespace uwb::client {

namespace {

using namespace uwb::protocol;

[[nodiscard]] ClientResult<ServiceResponse> serviceFailure(const PendingRequest &request, ClientErrorCode code,
                                                           const std::string &message) {
    ClientError error = makeError(ErrorDomain::Service, code, message);
    error.transactionId = request.transactionId;
    error.serviceId = request.requestSid;
    return ClientResult<ServiceResponse>::error(std::move(error));
}

// encodeFrame() is fallible only for oversized payloads; the queue keeps the
// unwrapped bytes and counts a drop if encoding ever fails.
[[nodiscard]] ByteBuffer frameBytes(PayloadType type, ConstBytes payload) {
    auto encoded = encodeFrame(type, payload);
    if (!encoded.ok()) {
        return {};
    }
    return std::move(encoded).value();
}

} // namespace

ClientConnection::ClientConnection(const ConnectionOptions &options, IClock &clock, ITaskExecutor &executor)
    : options_(options),
      clock_(clock),
      executor_(executor),
      parser_(kMaxProtocolPayload),
      pending_(options.maxOutstandingRequests),
      deviceUuid_(options.deviceUuid) {}

// ---------------------------------------------------------------------------
// Wiring
// ---------------------------------------------------------------------------

void ClientConnection::bindTransport(std::unique_ptr<IClientTransport> transport) {
    transport_ = std::move(transport);
}

void ClientConnection::setEventCallback(EventCallback callback) { eventCallback_ = std::move(callback); }
void ClientConnection::setSetupHandler(SetupCallback handler) { setupHandler_ = std::move(handler); }
void ClientConnection::setReconnectHandler(ReconnectCallback handler) { reconnectHandler_ = std::move(handler); }
void ClientConnection::setStateCallback(StateCallback handler) { stateCallback_ = std::move(handler); }
void ClientConnection::setErrorCallback(ErrorCallback handler) { errorCallback_ = std::move(handler); }

void ClientConnection::start() {
    // An explicit start restarts the reconnect backoff from the beginning (§73).
    reconnectAttempt_ = 0;
    reconnectDelayUs_ = 0;
    beginConnect();
}

// §73: a reconnect keeps the backoff state alive, otherwise a flaky device would
// be hammered at the initial delay forever.
void ClientConnection::reconnectNow() {
    beginConnect();
}

void ClientConnection::beginConnect() {
    manualClose_ = false;
    parser_.reset();
    txQueue_.clear();
    static_cast<void>(pending_.takeAll());
    activationOutstanding_ = false;
    reconnectAtUs_ = 0;
    aliveNonce_ = 0;
    aliveSentAtUs_ = 0;
    lastClientPresentUs_ = 0;
    lastActivityUs_ = clock_.nowUs();

    if (!transport_) {
        lastError_ = makeError(ErrorDomain::Network, ClientErrorCode::InvalidState, "connection has no transport bound");
        notifyError(lastError_);
        return;
    }

    setState(ConnectionState::Connecting);
    const std::uint32_t timeoutMs = options_.timeouts.connectTimeoutMs;
    transport_->connect(timeoutMs, [this](TransportStatus status, const std::string &message) {
        if (status == TransportStatus::Ok) {
            return; // onConnected() is invoked by the adapter when the socket is up.
        }
        const ClientErrorCode code = status == TransportStatus::ConnectFailed ? ClientErrorCode::ConnectFailed
                                                                             : ClientErrorCode::TransportFailure;
        lastError_ = makeError(ErrorDomain::Network, code, message);
        notifyError(lastError_);
        scheduleReconnect(clock_.nowUs());
    });
}

void ClientConnection::close() {
    manualClose_ = true;
    if (state_ == ConnectionState::Disconnected) {
        return;
    }
    setState(ConnectionState::Closing);
    for (PendingRequest &request : pending_.takeAll()) {
        if (request.completion) {
            request.completion(serviceFailure(request, ClientErrorCode::Cancelled, "connection closed by client"));
        }
    }
    txQueue_.clear();
    if (transport_) {
        transport_->close();
    }
    setState(ConnectionState::Disconnected);
}

// ---------------------------------------------------------------------------
// Transport adapter entry points
// ---------------------------------------------------------------------------

void ClientConnection::onConnected() {
    diagnostics_.lastActivityUs = clock_.nowUs();
    lastActivityUs_ = diagnostics_.lastActivityUs;
    setState(ConnectionState::TcpConnected);
    startActivation();
}

void ClientConnection::onBytes(ConstBytes chunk) {
    lastActivityUs_ = clock_.nowUs();
    diagnostics_.lastActivityUs = lastActivityUs_;

    auto pushed = parser_.push(chunk);
    if (!pushed.ok()) {
        const ProtocolError error = parser_.error().has_value() ? *parser_.error() : makeError(pushed.code());
        diagnostics_.protocolErrors++;
        lastError_ = makeError(ErrorDomain::Protocol, ClientErrorCode::FramingError,
                               std::string("framing error: ") + protocolErrorName(error.code));
        notifyError(lastError_);
        if (transport_) {
            transport_->close();
        }
        onClosed(TransportStatus::ProtocolViolation, lastError_.message);
        return;
    }

    while (pending_.size() <= pending_.capacity() && parser_.bufferedFrames() > 0) {
        auto frame = parser_.popFrame();
        if (!frame.ok()) {
            break;
        }
        handleFrame(frame.value());
    }
}

void ClientConnection::onClosed(TransportStatus status, const std::string &message) {
    if (state_ == ConnectionState::Disconnected) {
        return;
    }

    const bool wasManual = manualClose_;
    for (PendingRequest &request : pending_.takeAll()) {
        if (!request.completion) {
            continue;
        }
        if (status == TransportStatus::ProtocolViolation) {
            request.completion(serviceFailure(request, ClientErrorCode::ProtocolViolation, message));
        } else {
            request.completion(serviceFailure(request, ClientErrorCode::Disconnected, message));
        }
    }

    txQueue_.clear();
    parser_.reset();
    activationOutstanding_ = false;
    aliveNonce_ = 0;
    aliveSentAtUs_ = 0;
    role_ = ConnectionRole::None;
    session_ = SessionId::Default;
    effectiveMask_ = 0;
    serverLogicalAddress_ = kLogicalAddressInvalid;

    if (wasManual || status == TransportStatus::Closed) {
        if (wasManual) {
            setState(ConnectionState::Disconnected);
            return;
        }
    }

    if (!options_.autoReconnect || wasManual) {
        setState(ConnectionState::Disconnected);
        return;
    }

    scheduleReconnect(clock_.nowUs());
}

// ---------------------------------------------------------------------------
// Periodic supervision
// ---------------------------------------------------------------------------

void ClientConnection::tick(std::uint64_t nowUs) {
    diagnostics_.lastActivityUs = nowUs;

    if (state_ == ConnectionState::ReconnectWait) {
        if (reconnectAtUs_ != 0 && nowUs >= reconnectAtUs_) {
            reconnectAtUs_ = 0;
            reconnectAttempt_++;
            diagnostics_.reconnectAttempts = reconnectAttempt_;
            if (reconnectHandler_) {
                reconnectHandler_(*this);
            }
        }
        return;
    }

    if (!isActive()) {
        return;
    }

    superviseAliveCheck(nowUs);
    superviseClientPresent(nowUs);
    expirePending(nowUs);
    flushTxQueue();
}

// §19: the client probes an idle connection and treats a missing answer as a
// dead transport, which is the only way to notice a half-open TCP session.
void ClientConnection::superviseAliveCheck(std::uint64_t nowUs) {
    const std::uint64_t idleUs = static_cast<std::uint64_t>(options_.timeouts.aliveCheckIdleMs) * 1000ULL;

    if (aliveNonce_ != 0) {
        if (nowUs >= aliveSentAtUs_ + idleUs) {
            aliveNonce_ = 0;
            diagnostics_.requestTimeouts++;
            lastError_ = makeError(ErrorDomain::Network, ClientErrorCode::Timeout, "alive check response missing");
            notifyError(lastError_);
            if (transport_) {
                transport_->close();
            }
            onClosed(TransportStatus::Closed, lastError_.message);
        }
        return;
    }

    if (nowUs < lastActivityUs_ + idleUs) {
        return;
    }

    aliveNonceCounter_++;
    aliveNonce_ = aliveNonceCounter_;
    aliveSentAtUs_ = nowUs;

    const ByteBuffer body = encodeAliveCheckRequest(AliveCheckRequest{aliveNonce_});
    enqueueFrame(frameBytes(PayloadType::AliveCheckRequest, bytesOf(body)), true);
    flushTxQueue();
}

// §29: an extended session is maintained with periodic ClientPresent requests.
void ClientConnection::superviseClientPresent(std::uint64_t nowUs) {
    if (session_ != SessionId::Extended || state_ != ConnectionState::Ready) {
        return;
    }
    const std::uint64_t intervalUs = static_cast<std::uint64_t>(options_.timeouts.clientPresentIntervalMs) * 1000ULL;
    if (lastClientPresentUs_ != 0 && nowUs < lastClientPresentUs_ + intervalUs) {
        return;
    }
    lastClientPresentUs_ = nowUs;

    ServiceRequest request;
    request.sid = static_cast<std::uint8_t>(ServiceId::ClientPresent);
    ClientPresentRequest present;
    request.pdu = encodeClientPresentRequest(present);

    static_cast<void>(sendRequest(request, [](ClientResult<ServiceResponse> result) {
        // Session loss surfaces as a later service rejection; keep the cadence.
        static_cast<void>(result);
    }));
}

void ClientConnection::expirePending(std::uint64_t nowUs) {
    for (PendingRequest &request : pending_.takeExpired(nowUs)) {
        diagnostics_.requestTimeouts++;
        if (request.completion) {
            request.completion(serviceFailure(request, ClientErrorCode::Timeout, "request timed out"));
        }
    }
}

// ---------------------------------------------------------------------------
// Activation (§18)
// ---------------------------------------------------------------------------

void ClientConnection::startActivation() {
    if (!transport_ || state_ != ConnectionState::TcpConnected) {
        return;
    }

    ConnectionActivationRequest request;
    request.clientLogicalAddress = options_.clientLogicalAddress;
    request.requestedRole = options_.desiredRole;
    request.flags = 0;
    request.clientInstanceUuid = options_.clientInstanceUuid;

    const ByteBuffer body = encodeConnectionActivationRequest(request);
    activationOutstanding_ = true;
    enqueueFrame(frameBytes(PayloadType::ConnectionActivationRequest, bytesOf(body)), true);
    flushTxQueue();
}

// ---------------------------------------------------------------------------
// Frame dispatch
// ---------------------------------------------------------------------------

void ClientConnection::handleFrame(const Frame &frame) {
    diagnostics_.framesReceived++;

    switch (frame.type()) {
    case PayloadType::ApplicationMessage:
        handleApplicationMessage(frame);
        break;
    case PayloadType::ApplicationMessageAck:
        handleApplicationAck(frame);
        break;
    case PayloadType::ApplicationMessageNack:
        handleApplicationNack(frame);
        break;
    case PayloadType::EventNotification:
        handleEventNotification(frame);
        break;
    case PayloadType::ConnectionActivationResponse:
        handleActivationFrame(frame);
        break;
    case PayloadType::AliveCheckRequest:
    case PayloadType::AliveCheckResponse:
        handleAliveFrame(frame);
        break;
    case PayloadType::GenericHeaderNack: {
        diagnostics_.transportNacks++;
        auto nack = decodeGenericHeaderNack(frame.payloadBytes());
        const std::string reason = nack.ok() ? std::string("server transport nack 0x") +
                                                   std::to_string(static_cast<unsigned>(nack.value().code))
                                             : "malformed transport nack";
        lastError_ = makeError(ErrorDomain::Transport, ClientErrorCode::ProtocolViolation, reason);
        notifyError(lastError_);
        if (transport_) {
            transport_->close();
        }
        onClosed(TransportStatus::ProtocolViolation, reason);
        break;
    }
    case PayloadType::DeviceIdRequest:
    case PayloadType::DeviceIdResponse:
    case PayloadType::ConnectionActivationRequest: {
        // A client must not receive transport-direction payloads on TCP; this is
        // a protocol violation that invalidates the stream.
        diagnostics_.protocolErrors++;
        lastError_ = makeError(ErrorDomain::Protocol, ClientErrorCode::ProtocolViolation,
                               "unexpected payload type on TCP connection");
        notifyError(lastError_);
        if (transport_) {
            transport_->close();
        }
        onClosed(TransportStatus::ProtocolViolation, lastError_.message);
        break;
    }
    default: {
        diagnostics_.protocolErrors++;
        lastError_ = makeError(ErrorDomain::Protocol, ClientErrorCode::DecodeError, "unknown payload type");
        notifyError(lastError_);
        if (transport_) {
            transport_->close();
        }
        onClosed(TransportStatus::ProtocolViolation, lastError_.message);
        break;
    }
    }
}

void ClientConnection::handleActivationFrame(const Frame &frame) {
    auto decoded = decodeConnectionActivationResponse(frame.payloadBytes());
    if (!decoded.ok()) {
        diagnostics_.protocolErrors++;
        lastError_ = makeError(ErrorDomain::Protocol, ClientErrorCode::DecodeError,
                               std::string("malformed activation response: ") + protocolErrorName(decoded.code()));
        notifyError(lastError_);
        if (transport_) {
            transport_->close();
        }
        onClosed(TransportStatus::ProtocolViolation, lastError_.message);
        return;
    }

    const ConnectionActivationResponse &response = decoded.value();
    activationOutstanding_ = false;
    serverLogicalAddress_ = response.serverLogicalAddress;
    role_ = response.assignedRole;
    serverMaxPayload_ = response.serverMaxPayload;
    effectiveMask_ = response.capabilityMask;
    deviceUuid_ = domain::DeviceUuid{response.deviceUuid};

    switch (response.responseCode) {
    case ActivationResponseCode::AcceptedRequestedRole:
    case ActivationResponseCode::AcceptedDowngradedToObserver:
        setState(ConnectionState::Activated);
        if (setupHandler_) {
            setupHandler_(*this);
        }
        setState(ConnectionState::Ready);
        return;
    default:
        break;
    }

    // Activation rejection: the server closes the connection. Reconnecting would
    // repeat the same policy failure, so it is suppressed (§73).
    manualClose_ = true;
    lastError_ = makeError(ErrorDomain::Network, ClientErrorCode::ActivationRejected,
                           "activation rejected with code " + std::to_string(static_cast<unsigned>(response.responseCode)));
    notifyError(lastError_);
    if (transport_) {
        transport_->close();
    }
    onClosed(TransportStatus::Closed, lastError_.message);
    setState(ConnectionState::Disconnected);
}

void ClientConnection::handleAliveFrame(const Frame &frame) {
    if (frame.type() == PayloadType::AliveCheckRequest) {
        // Server-initiated probe: echo the nonce verbatim (§19).
        auto request = decodeAliveCheckRequest(frame.payloadBytes());
        if (!request.ok()) {
            diagnostics_.protocolErrors++;
            return;
        }
        const ByteBuffer body = encodeAliveCheckResponse(AliveCheckResponse{request.value().nonce});
        enqueueFrame(frameBytes(PayloadType::AliveCheckResponse, bytesOf(body)), true);
        flushTxQueue();
        return;
    }

    auto response = decodeAliveCheckResponse(frame.payloadBytes());
    if (!response.ok()) {
        diagnostics_.protocolErrors++;
        return;
    }
    if (aliveNonce_ == 0 || response.value().nonce != aliveNonce_) {
        diagnostics_.unmatchedResponses++;
        return;
    }
    aliveNonce_ = 0;
    lastActivityUs_ = clock_.nowUs();
}

void ClientConnection::handleApplicationMessage(const Frame &frame) {
    auto envelope = decodeApplicationEnvelope(frame.payloadBytes());
    if (!envelope.ok()) {
        diagnostics_.protocolErrors++;
        lastError_ = makeError(ErrorDomain::Protocol, ClientErrorCode::DecodeError,
                               std::string("malformed application envelope: ") + protocolErrorName(envelope.code()));
        notifyError(lastError_);
        return;
    }

    const ApplicationEnvelope &message = envelope.value();
    const ConstBytes pdu = message.serviceBytes();

    // §21.3: 0x78 ResponsePending keeps the transaction alive but pauses the
    // p2* deadline; it is never delivered as a final answer.
    if (pdu.size() >= 3 && pdu[0] == kNegativeResponseSid && pdu[2] == static_cast<std::uint8_t>(ServiceNrc::ResponsePending)) {
        PendingRequest *entry = pending_.find(message.transactionId);
        if (entry == nullptr) {
            pending_.recordLateResponse();
            diagnostics_.lateResponses++;
            return;
        }
        // Re-arm the p2* deadline and keep the transaction outstanding (§21.3).
        entry->sawResponsePending = true;
        entry->deadlineUs = clock_.nowUs() + static_cast<std::uint64_t>(requestTimeoutMs(0)) * 1000ULL;
        return;
    }

    auto pending = pending_.take(message.transactionId);
    if (pending.transactionId == message.transactionId && pending.completion) {
        ServiceResponse response;
        response.transactionId = message.transactionId;
        response.requestSid = pending.requestSid != 0 ? pending.requestSid : message.servicePdu[0];
        response.negative = !message.servicePdu.empty() && message.servicePdu[0] == kNegativeResponseSid;
        response.pdu = message.servicePdu;

        if (response.negative && message.servicePdu.size() >= 3) {
            response.nrc = static_cast<ServiceNrc>(message.servicePdu[2]);
            diagnostics_.negativeResponses++;

            char nrcText[8];
            std::snprintf(nrcText, sizeof(nrcText), "%02x", static_cast<unsigned>(message.servicePdu[2]));
            ClientError error = makeError(ErrorDomain::Service, ClientErrorCode::ServiceRejected,
                                          std::string("service rejected with NRC 0x") + nrcText);
            error.transactionId = message.transactionId;
            error.serviceId = response.requestSid;
            error.nrc = message.servicePdu[2];
            pending.completion(ClientResult<ServiceResponse>::error(std::move(error)));
            return;
        }

        diagnostics_.responsesMatched++;
        pending.completion(ClientResult<ServiceResponse>::ok(std::move(response)));
        return;
    }

    // Unknown transaction id: count it, never deliver it (§74 category 2).
    pending_.recordLateResponse();
    diagnostics_.lateResponses++;
    diagnostics_.unmatchedResponses++;
}

void ClientConnection::handleApplicationAck(const Frame &frame) {
    auto ack = decodeApplicationMessageAck(frame.payloadBytes());
    if (!ack.ok()) {
        diagnostics_.protocolErrors++;
        return;
    }
    diagnostics_.acksReceived++;
    if (auto *pending = pending_.find(ack.value().transactionId); pending != nullptr) {
        pending->sent = true;
    }
}

void ClientConnection::handleApplicationNack(const Frame &frame) {
    auto nack = decodeApplicationMessageNack(frame.payloadBytes());
    if (!nack.ok()) {
        diagnostics_.protocolErrors++;
        return;
    }

    diagnostics_.transportNacks++;
    auto pending = pending_.take(nack.value().transactionId);
    if (pending.transactionId == 0) {
        pending_.recordLateResponse();
        return;
    }
    if (!pending.completion) {
        return;
    }

    ClientError error = makeError(ErrorDomain::Transport, ClientErrorCode::ServiceRejected,
                                  std::string("transport NACK code ") + std::to_string(static_cast<unsigned>(nack.value().code)));
    error.transactionId = nack.value().transactionId;
    error.serviceId = pending.requestSid;
    pending.completion(ClientResult<ServiceResponse>::error(std::move(error)));
}

void ClientConnection::handleEventNotification(const Frame &frame) {
    auto event = decodeEventNotification(frame.payloadBytes());
    if (!event.ok()) {
        diagnostics_.protocolErrors++;
        lastError_ = makeError(ErrorDomain::Protocol, ClientErrorCode::DecodeError,
                               std::string("malformed event notification: ") + protocolErrorName(event.code()));
        notifyError(lastError_);
        return;
    }

    diagnostics_.eventNotifications++;

    if (static_cast<EventId>(event.value().eventId) == EventId::StreamStatus) {
        auto status = decodeStreamStatusEvent(event.value().payloadBytes());
        if (status.ok()) {
            updateSubscriptionFromStatus(status.value());
        }
    }

    if (!eventCallback_) {
        return;
    }
    EventDelivery delivery;
    delivery.device = deviceUuid_;
    delivery.event = event.value();
    eventCallback_(delivery);
}

void ClientConnection::updateSubscriptionFromStatus(const StreamStatusEvent &status) {
    for (StreamSubscription &subscription : subscriptions_) {
        if (subscription.streamId != status.streamId) {
            continue;
        }
        subscription.state = static_cast<StreamState>(status.state);
        subscription.droppedCount = status.droppedCount;
        return;
    }
}

// ---------------------------------------------------------------------------
// Requests (plan §25.2)
// ---------------------------------------------------------------------------

std::uint32_t ClientConnection::requestTimeoutMs(std::uint32_t requestedMs) const noexcept {
    if (requestedMs != 0) {
        return requestedMs;
    }
    return options_.timeouts.requestTimeoutMs;
}

ClientResult<RequestId> ClientConnection::sendRequest(const ServiceRequest &request, ResponseCallback completion) {
    if (!isActive()) {
        return ClientResult<RequestId>::error(ErrorDomain::Network, ClientErrorCode::NotReady,
                                              std::string("connection is ") + connectionStateName(state_));
    }
    if (pending_.full()) {
        return ClientResult<RequestId>::error(ErrorDomain::Service, ClientErrorCode::TooManyOutstanding,
                                              "too many outstanding requests on this connection");
    }

    auto transactionId = transactions_.allocate([this](RequestId id) { return pending_.inUse(id); });
    if (!transactionId.has_value()) {
        return ClientResult<RequestId>::error(ErrorDomain::Service, ClientErrorCode::TooManyOutstanding,
                                              "transaction id space exhausted");
    }

    ApplicationEnvelope envelope;
    envelope.sourceLogicalAddress = options_.clientLogicalAddress;
    envelope.targetLogicalAddress = serverLogicalAddress_;
    envelope.transactionId = *transactionId;
    envelope.flags = request.ackRequired ? kApplicationFlagAckRequired : 0;
    envelope.servicePdu = request.pdu;

    PendingRequest pending;
    pending.transactionId = *transactionId;
    pending.requestSid = request.sid;
    pending.sent = !request.ackRequired;
    pending.completion = std::move(completion);

    diagnostics_.requestsSent++;
    if (!pending_.insert(std::move(pending))) {
        return ClientResult<RequestId>::error(ErrorDomain::Service, ClientErrorCode::TooManyOutstanding,
                                              "pending request table is full");
    }

    {
        PendingRequest *entry = pending_.find(*transactionId);
        if (entry != nullptr) {
            entry->sentAtUs = clock_.nowUs();
            entry->deadlineUs = entry->sentAtUs + static_cast<std::uint64_t>(requestTimeoutMs(request.timeoutMs)) * 1000ULL;
        }
    }

    const ByteBuffer body = encodeApplicationEnvelope(envelope);
    enqueueFrame(frameBytes(PayloadType::ApplicationMessage, bytesOf(body)), false);
    flushTxQueue();
    return ClientResult<RequestId>::ok(*transactionId);
}

void ClientConnection::cancelRequest(RequestId transactionId) {
    auto pending = pending_.take(transactionId);
    if (pending.transactionId == 0) {
        return;
    }
    if (pending.completion) {
        pending.completion(serviceFailure(pending, ClientErrorCode::Cancelled, "request cancelled"));
    }
}

// ---------------------------------------------------------------------------
// TX queue (§53: bounded, control frames prioritised, drops counted)
// ---------------------------------------------------------------------------

void ClientConnection::enqueueFrame(ByteBuffer frame, bool front) {
    if (txQueue_.size() >= options_.maxTxQueueFrames) {
        // Drop the oldest queued frame: control frames go to the front, so the
        // oldest entry is always the least important one.
        txQueue_.pop_front();
        diagnostics_.txQueueDrops++;
    }
    if (front) {
        txQueue_.push_front(std::move(frame));
    } else {
        txQueue_.push_back(std::move(frame));
    }
}

void ClientConnection::flushTxQueue() {
    if (!transport_ || !transport_->isOpen()) {
        return;
    }
    while (!txQueue_.empty()) {
        ByteBuffer frame = std::move(txQueue_.front());
        txQueue_.pop_front();
        diagnostics_.framesSent++;
        transport_->write(ConstBytes{frame.data(), frame.size()});
    }
}

// ---------------------------------------------------------------------------
// Reconnect backoff (specification §73)
// ---------------------------------------------------------------------------

void ClientConnection::scheduleReconnect(std::uint64_t nowUs) {
    if (!options_.autoReconnect || manualClose_) {
        setState(ConnectionState::Disconnected);
        return;
    }

    std::uint64_t delayUs = 0;
    if (reconnectAttempt_ == 0) {
        reconnectDelayUs_ = static_cast<std::uint64_t>(options_.timeouts.reconnectInitialDelayMs) * 1000ULL;
    } else {
        reconnectDelayUs_ *= 2ULL;
        const std::uint64_t maxUs = static_cast<std::uint64_t>(options_.timeouts.reconnectMaxDelayMs) * 1000ULL;
        if (reconnectDelayUs_ > maxUs) {
            reconnectDelayUs_ = maxUs;
        }
    }
    if (options_.reconnectJitter) {
        delayUs = options_.reconnectJitter(static_cast<std::uint32_t>(reconnectDelayUs_));
    }
    reconnectDelayUs_ += delayUs;

    reconnectAtUs_ = nowUs + reconnectDelayUs_;
    setState(ConnectionState::ReconnectWait);
}

// ---------------------------------------------------------------------------
// Misc
// ---------------------------------------------------------------------------

void ClientConnection::markReady() {
    if (isActive()) {
        setState(ConnectionState::Ready);
    }
}

void ClientConnection::addSubscription(const StreamSubscription &subscription) {
    for (StreamSubscription &existing : subscriptions_) {
        if (existing.streamId == subscription.streamId) {
            existing = subscription;
            return;
        }
    }
    subscriptions_.push_back(subscription);
}

void ClientConnection::removeSubscription(std::uint16_t streamId) {
    for (std::size_t index = 0; index < subscriptions_.size(); ++index) {
        if (subscriptions_[index].streamId == streamId) {
            subscriptions_.erase(subscriptions_.begin() + static_cast<std::ptrdiff_t>(index));
            return;
        }
    }
}

void ClientConnection::clearSubscriptions() { subscriptions_.clear(); }

void ClientConnection::completeTransaction(RequestId transactionId, ClientResult<ServiceResponse> result) {
    auto pending = pending_.take(transactionId);
    if (pending.transactionId == 0 && pending.completion) {
        pending_.recordLateResponse();
        return;
    }
    if (pending.completion) {
        pending.completion(std::move(result));
    }
}

void ClientConnection::notifyError(const ClientError &error) {
    if (errorCallback_) {
        errorCallback_(*this, error);
    }
}

void ClientConnection::setState(ConnectionState state) {
    if (state_ == state) {
        return;
    }
    state_ = state;
    if (stateCallback_) {
        stateCallback_(*this);
    }
}

} // namespace uwb::client
