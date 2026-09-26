#include "uwb/server/service_dispatcher.hpp"

#include <algorithm>
#include <tuple>
#include <utility>

#include "uwb/protocol/constants.hpp"
#include "uwb/protocol/generic_header.hpp"
#include "uwb/protocol/payload_types.hpp"

namespace uwb::server {

using uwb::protocol::ApplicationEnvelope;
using uwb::protocol::ApplicationMessageAck;
using uwb::protocol::ApplicationMessageNack;
using uwb::protocol::ApplicationNackCode;
using uwb::protocol::ByteBuffer;
using uwb::protocol::ConstBytes;
using uwb::protocol::ConnectionActivationRequest;
using uwb::protocol::ConnectionActivationResponse;
using uwb::protocol::Did;
using uwb::protocol::DeviceResetResponse;
using uwb::protocol::DeviceResetType;
using uwb::protocol::DeviceResetRequest;
using uwb::protocol::EventQueryResponse;
using uwb::protocol::EventSubscribeResponse;
using uwb::protocol::EventUnsubscribeResponse;
using uwb::protocol::GenericHeaderNack;
using uwb::protocol::GenericHeaderNackCode;
using uwb::protocol::PayloadType;
using uwb::protocol::EventSubscribeRequest;
using uwb::protocol::ReadDidResponse;
using uwb::protocol::RoutineControlRequest;
using uwb::protocol::RoutineControlResponse;
using uwb::protocol::RoutineControlType;
using uwb::protocol::RoutineId;
using uwb::protocol::RoutineState;
using uwb::protocol::SecurityAccessRequest;
using uwb::protocol::SecurityAccessResponse;
using uwb::protocol::SecuritySubFunction;
using uwb::protocol::ServiceId;
using uwb::protocol::ServiceNrc;
using uwb::protocol::SessionControlResponse;
using uwb::protocol::SessionId;
using uwb::protocol::WriteDidResponse;

namespace {
// §27 and §21.4: a structurally invalid service PDU is reported as 0x13, a
// syntactically valid value outside the supported range as 0x31.
[[nodiscard]] ServiceNrc nrcForDecodeError(uwb::protocol::ProtocolErrorCode code) noexcept {
    return code == uwb::protocol::ProtocolErrorCode::InvalidField ? ServiceNrc::RequestOutOfRange
                                                                  : ServiceNrc::IncorrectMessageLengthOrInvalidFormat;
}
} // namespace

namespace {

// Delay between transmitting the reset response and executing the reset (§24).
inline constexpr std::uint64_t kResetDelayAfterResponseMs = 100;

std::uint64_t p2StarUs(const ServerConfig &config) noexcept {
    return msToUs(static_cast<std::uint64_t>(config.p2StarServerMax10ms) * 10ULL);
}

void pushFrame(ConnectionTxQueue &tx, PayloadType type, ConstBytes payload, TxPriority priority) {
    auto frame = uwb::protocol::encodeFrame(type, payload);
    if (!frame.ok()) {
        return;
    }
    std::ignore = tx.push(uwb::protocol::bytesOf(frame.value()), priority);
}

void pushHeaderNack(ConnectionTxQueue &tx, GenericHeaderNackCode code) {
    const ByteBuffer payload = uwb::protocol::encodeGenericHeaderNack(GenericHeaderNack{code});
    pushFrame(tx, PayloadType::GenericHeaderNack, uwb::protocol::bytesOf(payload), TxPriority::High);
}

} // namespace

// ---------------------------------------------------------------------------
// ConnectionTxStore
// ---------------------------------------------------------------------------

ConnectionTxQueue &ConnectionTxStore::getOrCreate(ConnectionId id) {
    auto it = queues_.find(id);
    if (it != queues_.end()) {
        return it->second;
    }
    auto result = queues_.emplace(id, ConnectionTxQueue{highCapacity_, normalCapacity_});
    return result.first->second;
}

ConnectionTxQueue *ConnectionTxStore::find(ConnectionId id) noexcept {
    auto it = queues_.find(id);
    return it == queues_.end() ? nullptr : &it->second;
}

void ConnectionTxStore::remove(ConnectionId id) noexcept { queues_.erase(id); }

// ---------------------------------------------------------------------------
// Dispatcher: response plumbing
// ---------------------------------------------------------------------------

ServiceDispatcher::ServiceDispatcher(Dependencies &deps) noexcept : deps_(deps) {}

void ServiceDispatcher::respondPdu(ConnectionId connection, std::uint16_t clientAddress,
                                   std::uint32_t transactionId, ConstBytes servicePdu, TxPriority priority) {
    ConnectionTxQueue *tx = deps_.tx.find(connection);
    if (tx == nullptr) {
        return;
    }

    ApplicationEnvelope envelope;
    envelope.sourceLogicalAddress = deps_.dids.logicalAddress();
    envelope.targetLogicalAddress = clientAddress;
    envelope.transactionId = transactionId;
    envelope.flags = 0;
    envelope.servicePdu.assign(servicePdu.begin(), servicePdu.end());

    const ByteBuffer payload = uwb::protocol::encodeApplicationEnvelope(envelope);
    pushFrame(*tx, PayloadType::ApplicationMessage, uwb::protocol::bytesOf(payload), priority);
}

void ServiceDispatcher::respondNegative(ConnectionId connection, const Request &req, ServiceNrc nrc) {
    const ByteBuffer pdu = uwb::protocol::encodeNegativeResponse(req.sid, nrc);
    respondPdu(connection, req.clientAddress, req.transactionId, uwb::protocol::bytesOf(pdu), TxPriority::High);
}

void ServiceDispatcher::respondStatus(ConnectionId connection, const Request &req, ServerStatus status) {
    const auto nrc = nrcFor(status);
    respondNegative(connection, req, nrc.has_value() ? *nrc : ServiceNrc::GeneralReject);
}

void ServiceDispatcher::respondPending(ConnectionId connection, const Request &req) {
    // §21.4: the final response arrives later with the same transaction id.
    respondNegative(connection, req, ServiceNrc::ResponsePending);
}

void ServiceDispatcher::enqueueAck(ConnectionId connection, const Request &req) {
    ConnectionTxQueue *tx = deps_.tx.find(connection);
    if (tx == nullptr) {
        return;
    }
    ApplicationMessageAck ack;
    ack.sourceLogicalAddress = deps_.dids.logicalAddress();
    ack.targetLogicalAddress = req.clientAddress;
    ack.transactionId = req.transactionId;
    const ByteBuffer payload = uwb::protocol::encodeApplicationMessageAck(ack);
    pushFrame(*tx, PayloadType::ApplicationMessageAck, uwb::protocol::bytesOf(payload), TxPriority::High);
}

void ServiceDispatcher::enqueueNack(ConnectionId connection, const Request &req, ApplicationNackCode code) {
    ConnectionTxQueue *tx = deps_.tx.find(connection);
    if (tx == nullptr) {
        return;
    }
    ApplicationMessageNack nack;
    nack.sourceLogicalAddress = deps_.dids.logicalAddress();
    nack.targetLogicalAddress = req.clientAddress;
    nack.transactionId = req.transactionId;
    nack.code = code;
    const ByteBuffer payload = uwb::protocol::encodeApplicationMessageNack(nack);
    pushFrame(*tx, PayloadType::ApplicationMessageNack, uwb::protocol::bytesOf(payload), TxPriority::High);
}

// ---------------------------------------------------------------------------
// Frame routing
// ---------------------------------------------------------------------------

void ServiceDispatcher::handleFrame(ConnectionId connection, const Frame &frame) {
    ConnectionTxQueue *tx = deps_.tx.find(connection);
    if (tx == nullptr || deps_.connections.find(connection) == nullptr) {
        return;
    }

    const auto type = frame.type();
    switch (type) {
    case PayloadType::ConnectionActivationRequest:
        handleActivation(connection, frame, *tx);
        return;
    case PayloadType::AliveCheckRequest:
        handleAliveCheck(connection, frame, *tx);
        return;
    case PayloadType::ApplicationMessage:
        handleApplicationMessage(connection, frame, *tx);
        return;
    case PayloadType::DeviceIdRequest:
    case PayloadType::DeviceIdResponse:
        // §8, §14: device identification belongs to the UDP discovery layer.
        pushHeaderNack(*tx, GenericHeaderNackCode::InvalidInCurrentState);
        deps_.connections.markClosing(connection);
        return;
    case PayloadType::ApplicationMessageAck:
    case PayloadType::ApplicationMessageNack:
    case PayloadType::EventNotification:
        // Server-to-client payload types: a client must not send them.
        pushHeaderNack(*tx, GenericHeaderNackCode::InvalidInCurrentState);
        deps_.connections.markClosing(connection);
        return;
    case PayloadType::GenericHeaderNack:
        pushHeaderNack(*tx, GenericHeaderNackCode::IncorrectHeaderPattern);
        deps_.connections.markClosing(connection);
        return;
    default:
        pushHeaderNack(*tx, GenericHeaderNackCode::UnknownPayloadType);
        deps_.connections.markClosing(connection);
        return;
    }
}

// ---------------------------------------------------------------------------
// Connection Activation (0x0005 / 0x0006, specification §18)
// ---------------------------------------------------------------------------

void ServiceDispatcher::handleActivation(ConnectionId connection, const Frame &frame, ConnectionTxQueue &tx) {
    auto request = uwb::protocol::decodeConnectionActivationRequest(frame.payloadBytes());

    ConnectionActivationResponse response;
    response.serverLogicalAddress = deps_.dids.logicalAddress();
    response.assignedRole = ConnectionRole::None;
    response.serverMaxPayload = uwb::protocol::kMaxProtocolPayload;
    response.capabilityMask = deps_.dids.effectiveCapabilities();
    if (auto uuid = deps_.dids.read(Did::DeviceUuid); uuid.ok()) {
        const ConstBytes bytes = uwb::protocol::bytesOf(uuid.value);
        for (std::size_t i = 0; i < bytes.size() && i < response.deviceUuid.bytes.size(); ++i) {
            response.deviceUuid.bytes[i] = bytes[i];
        }
    }

    if (!request.ok()) {
        response.responseCode = ActivationResponseCode::RejectedRolePolicyDenied;
        pushFrame(tx, PayloadType::ConnectionActivationResponse, uwb::protocol::bytesOf(
            uwb::protocol::encodeConnectionActivationResponse(response)), TxPriority::High);
        deps_.connections.markClosing(connection); // §18: respond, then close
        return;
    }

    const ConnectionActivationRequest &activation = request.value();
    if (!uwb::protocol::isClientLogicalAddress(activation.clientLogicalAddress)) {
        // §40.4: clients live in 0x0E00..0x0EFF.
        response.responseCode = ActivationResponseCode::RejectedRolePolicyDenied;
        pushFrame(tx, PayloadType::ConnectionActivationResponse,
                  uwb::protocol::bytesOf(uwb::protocol::encodeConnectionActivationResponse(response)),
                  TxPriority::High);
        deps_.connections.markClosing(connection);
        return;
    }

    RoleDecision decision = deps_.connections.assignRole(connection, activation.requestedRole,
                                                        deps_.clock.monotonicUs());
    response.assignedRole = decision.assigned;
    response.responseCode = decision.code;

    if (decision.accepted) {
        ConnectionContext *ctx = deps_.connections.find(connection);
        if (ctx != nullptr) {
            ctx->clientLogicalAddress = activation.clientLogicalAddress;
            ctx->clientInstanceUuid.assign(activation.clientInstanceUuid.bytes.begin(),
                                           activation.clientInstanceUuid.bytes.end());
            ctx->deviceUuid.assign(response.deviceUuid.bytes.begin(), response.deviceUuid.bytes.end());
        }
    }

    const ByteBuffer payload = uwb::protocol::encodeConnectionActivationResponse(response);
    pushFrame(tx, PayloadType::ConnectionActivationResponse, uwb::protocol::bytesOf(payload), TxPriority::High);

    if (!decision.accepted) {
        deps_.connections.markClosing(connection); // §18: transmit the response, then close
    }
}

// ---------------------------------------------------------------------------
// Alive Check (0x0007 / 0x0008, specification §19)
// ---------------------------------------------------------------------------

void ServiceDispatcher::handleAliveCheck(ConnectionId connection, const Frame &frame, ConnectionTxQueue &tx) {
    auto request = uwb::protocol::decodeAliveCheckRequest(frame.payloadBytes());
    if (!request.ok()) {
        // Malformed keepalive: ignored; the TCP idle timer closes the connection.
        return;
    }

    const ByteBuffer payload =
        uwb::protocol::encodeAliveCheckResponse(uwb::protocol::AliveCheckResponse{request.value().nonce});
    pushFrame(tx, PayloadType::AliveCheckResponse, uwb::protocol::bytesOf(payload), TxPriority::High);
    deps_.connections.touch(connection, deps_.clock.monotonicUs());
}

// ---------------------------------------------------------------------------
// Application Message (0x8001, specification §20)
// ---------------------------------------------------------------------------

void ServiceDispatcher::handleApplicationMessage(ConnectionId connection, const Frame &frame,
                                                 ConnectionTxQueue &tx) {
    (void)tx;
    auto envelope = uwb::protocol::decodeApplicationEnvelope(frame.payloadBytes());

    Request req;
    if (!envelope.ok()) {
        enqueueNack(connection, req, ApplicationNackCode::InvalidEnvelopeLength);
        return;
    }

    const ApplicationEnvelope &message = envelope.value();
    req.clientAddress = message.sourceLogicalAddress;
    req.transactionId = message.transactionId;
    req.pdu = message.serviceBytes();

    ConnectionContext *ctx = deps_.connections.find(connection);
    if (ctx == nullptr) {
        return;
    }

    if (message.flags & uwb::protocol::kApplicationFlagReservedMask) {
        enqueueNack(connection, req, ApplicationNackCode::InvalidEnvelopeFlags);
        return;
    }
    if (!uwb::protocol::isClientLogicalAddress(message.sourceLogicalAddress) ||
        message.targetLogicalAddress != deps_.dids.logicalAddress()) {
        enqueueNack(connection, req, ApplicationNackCode::InvalidSourceOrTargetAddress);
        return;
    }
    if (!ctx->isActive()) {
        // §18: until activation only Activation and Alive Check frames are valid.
        enqueueNack(connection, req, ApplicationNackCode::ConnectionNotActivated);
        deps_.connections.markClosing(connection);
        return;
    }

    auto pdu = uwb::protocol::decodeServicePdu(req.pdu);
    if (!pdu.ok()) {
        if (pdu.code() == uwb::protocol::ProtocolErrorCode::UnknownServiceId) {
            req.sid = req.pdu.empty() ? 0 : req.pdu[0];
            respondNegative(connection, req, ServiceNrc::ServiceNotSupported); // §21.4
        } else {
            enqueueNack(connection, req, ApplicationNackCode::InvalidEnvelopeLength);
        }
        return;
    }

    req.sid = pdu.value().sid;

    if (message.ackRequired()) {
        // §20.2: the ACK confirms a syntactically accepted envelope.
        enqueueAck(connection, req);
    }

    dispatchService(connection, req);
}

void ServiceDispatcher::dispatchService(ConnectionId connection, const Request &req) {
    // Any accepted application request resets the activity timers (§19, §22.3).
    deps_.connections.touch(connection, deps_.clock.monotonicUs());

    switch (static_cast<ServiceId>(req.sid)) {
    case ServiceId::SessionControl:
        handleSessionControl(connection, req);
        return;
    case ServiceId::PicoDeviceReset:
        handleDeviceReset(connection, req);
        return;
    case ServiceId::ReadDataByIdentifier:
        handleReadDid(connection, req);
        return;
    case ServiceId::SecurityAccess:
        handleSecurityAccess(connection, req);
        return;
    case ServiceId::WriteDataByIdentifier:
        handleWriteDid(connection, req);
        return;
    case ServiceId::RoutineControl:
        handleRoutineControl(connection, req);
        return;
    case ServiceId::ClientPresent:
        handleClientPresent(connection, req);
        return;
    case ServiceId::ExecuteAtCommand:
        handleAtCommand(connection, req);
        return;
    case ServiceId::EventControl:
        handleEventControl(connection, req);
        return;
    }
    respondNegative(connection, req, ServiceNrc::ServiceNotSupported);
}

// ---------------------------------------------------------------------------
// 0x10 Session Control (specification §23)
// ---------------------------------------------------------------------------

void ServiceDispatcher::handleSessionControl(ConnectionId connection, const Request &req) {
    auto request = uwb::protocol::decodeSessionControlRequest(req.pdu);
    if (!request.ok()) {
        respondNegative(connection, req, nrcForDecodeError(request.code()));
        return;
    }

    ConnectionContext *ctx = deps_.connections.find(connection);
    if (ctx == nullptr) {
        return;
    }

    const bool wantsExtended =
        request.value().requestedSession == static_cast<std::uint8_t>(SessionId::Extended);

    SessionControlResponse response;
    if (wantsExtended) {
        if (!ctx->isControl()) {
            respondNegative(connection, req, ServiceNrc::ConditionsNotCorrect); // §23: NRC 0x22
            return;
        }
        if (deps_.config.securityEnabled && !ctx->isUnlocked()) {
            respondNegative(connection, req, ServiceNrc::SecurityAccessDenied); // §23 + §25
            return;
        }
        deps_.connections.enterExtendedSession(connection, deps_.clock.monotonicUs());
        response.activeSession = static_cast<std::uint8_t>(SessionId::Extended);
    } else {
        deps_.connections.enterDefaultSession(connection); // clears security (§22.2)
        if (deps_.security != nullptr) {
            deps_.security->reset(connection);
        }
        response.activeSession = static_cast<std::uint8_t>(SessionId::Default);
    }

    response.p2ServerMaxMs = deps_.config.p2ServerMaxMs;
    response.p2StarServerMax10ms = deps_.config.p2StarServerMax10ms;

    const ByteBuffer pdu = uwb::protocol::encodeSessionControlResponse(response);
    respondPdu(connection, req.clientAddress, req.transactionId, uwb::protocol::bytesOf(pdu), TxPriority::High);
}

// ---------------------------------------------------------------------------
// 0x11 Pico Device Reset (specification §24)
// ---------------------------------------------------------------------------

void ServiceDispatcher::handleDeviceReset(ConnectionId connection, const Request &req) {
    auto request = uwb::protocol::decodeDeviceResetRequest(req.pdu);
    if (!request.ok()) {
        respondNegative(connection, req, ServiceNrc::IncorrectMessageLengthOrInvalidFormat);
        return;
    }

    const ConnectionContext *ctx = deps_.connections.find(connection);
    if (ctx == nullptr) {
        return;
    }
    if (!ctx->isControl() || !ctx->isExtendedSession()) {
        respondNegative(connection, req, ServiceNrc::ConditionsNotCorrect); // §24
        return;
    }
    if (deps_.config.securityEnabled && !ctx->isUnlocked()) {
        respondNegative(connection, req, ServiceNrc::SecurityAccessDenied); // §24
        return;
    }

    const DeviceResetResponse response{request.value().resetType};
    const ByteBuffer pdu = uwb::protocol::encodeDeviceResetResponse(response);
    respondPdu(connection, req.clientAddress, req.transactionId, uwb::protocol::bytesOf(pdu), TxPriority::High);

    // §24: transmit the response first, reset after a short delay.
    hostAction_.resetPico = true;
    hostAction_.hardReset = request.value().resetType == static_cast<std::uint8_t>(DeviceResetType::Hard);
    hostAction_.resetAfterUs = deps_.clock.monotonicUs() + msToUs(kResetDelayAfterResponseMs);
}

// ---------------------------------------------------------------------------
// 0x27 Security Access (specification §25)
// ---------------------------------------------------------------------------

void ServiceDispatcher::handleSecurityAccess(ConnectionId connection, const Request &req) {
    if (!deps_.config.securityEnabled || deps_.security == nullptr) {
        // §25: when disabled, CAP_SECURITY_ACCESS is clear and the service is refused.
        respondNegative(connection, req, ServiceNrc::ServiceNotSupportedInActiveSession);
        return;
    }

    ConnectionContext *ctx = deps_.connections.find(connection);
    if (ctx == nullptr) {
        return;
    }
    if (!ctx->isControl()) {
        respondNegative(connection, req, ServiceNrc::ConditionsNotCorrect); // §9.2
        return;
    }

    auto request = uwb::protocol::decodeSecurityAccessRequest(req.pdu);
    if (!request.ok()) {
        respondNegative(connection, req, ServiceNrc::IncorrectMessageLengthOrInvalidFormat);
        return;
    }

    const SecurityAccessRequest &access = request.value();

    if (access.subFunction == static_cast<std::uint8_t>(SecuritySubFunction::RequestSeed)) {
        auto seed = deps_.security->createSeed(connection);
        if (!seed.ok()) {
            respondStatus(connection, req, fromBackendStatus(BackendStatus::Failed));
            return;
        }
        ctx->security = SecurityState::SeedIssued;
        ctx->seed = seed.value();
        ctx->seedIssuedUs = deps_.clock.monotonicUs();

        const ByteBuffer pdu = uwb::protocol::encodeSecurityAccessResponse(
            SecurityAccessResponse{static_cast<std::uint8_t>(SecuritySubFunction::RequestSeed), seed.value()});
        respondPdu(connection, req.clientAddress, req.transactionId, uwb::protocol::bytesOf(pdu), TxPriority::High);
        return;
    }

    // SendKey
    if (ctx->security != SecurityState::SeedIssued) {
        respondNegative(connection, req, ServiceNrc::RequestSequenceError); // §21.4
        return;
    }

    const SecurityOutcome outcome = deps_.security->verifyKey(connection, ctx->seed, access.key);
    if (outcome.decision == SecurityDecision::Accepted) {
        ctx->security = SecurityState::Unlocked;
        ctx->failedAttempts = 0;
        ctx->seed = 0;
        const ByteBuffer pdu = uwb::protocol::encodeSecurityAccessResponse(
            SecurityAccessResponse{static_cast<std::uint8_t>(SecuritySubFunction::SendKey), 0});
        respondPdu(connection, req.clientAddress, req.transactionId, uwb::protocol::bytesOf(pdu), TxPriority::High);
        return;
    }

    deps_.security->noteFailure(connection);
    ctx->security = SecurityState::Locked;
    ctx->seed = 0;
    ctx->failedAttempts += 1;
    if (outcome.retryAfterMs > 0) {
        ctx->blockedUntilUs = deps_.clock.monotonicUs() + msToUs(outcome.retryAfterMs);
    }
    respondNegative(connection, req, nrcForSecurity(outcome.decision));
}

// ---------------------------------------------------------------------------
// 0x22 Read Data By Identifier (specification §26)
// ---------------------------------------------------------------------------

void ServiceDispatcher::handleReadDid(ConnectionId connection, const Request &req) {
    auto request = uwb::protocol::decodeReadDidRequest(req.pdu);
    if (!request.ok()) {
        respondNegative(connection, req, nrcForDecodeError(request.code()));
        return;
    }

    const std::uint16_t didValue = request.value().did;
    if (!uwb::protocol::isKnownDid(didValue)) {
        respondNegative(connection, req, ServiceNrc::RequestOutOfRange); // §26: NRC 0x31
        return;
    }
    const Did did = static_cast<Did>(didValue);

    auto result = deps_.dids.read(did);
    if (result.ok()) {
        const ReadDidResponse response{didValue, std::move(result.value)};
        const ByteBuffer pdu = uwb::protocol::encodeReadDidResponse(response);
        respondPdu(connection, req.clientAddress, req.transactionId, uwb::protocol::bytesOf(pdu), TxPriority::High);
        return;
    }

    if (result.status == ServerStatus::NeedsBackend) {
        if (deps_.backend == nullptr) {
            respondNegative(connection, req, ServiceNrc::ConditionsNotCorrect);
            return;
        }
        Pending pending;
        pending.connection = connection;
        pending.requestSid = req.sid;
        pending.clientAddress = req.clientAddress;
        pending.transactionId = req.transactionId;
        pending.did = didValue;
        pending.deadlineUs = deps_.clock.monotonicUs() + p2StarUs(deps_.config);
        const std::uint32_t token = addPending(pending);

        auto submitted = deps_.backend->readDid(connection, did,
                                                [this, token](DidReadResult done) { completeDidRead(token, done); });
        if (submitted.ok()) {
            setPendingOperation(token, submitted.value());
        }
        if (!submitted.ok()) {
            removePending(token);
            respondNegative(connection, req, deps_.backend->busy() ? ServiceNrc::BusyRepeatRequest
                                                                   : ServiceNrc::ConditionsNotCorrect);
            return;
        }
        respondPending(connection, req); // §21.4 NRC 0x78
        return;
    }

    respondStatus(connection, req, result.status);
}

// ---------------------------------------------------------------------------
// 0x2E Write Data By Identifier (specification §27, §39)
// ---------------------------------------------------------------------------

void ServiceDispatcher::handleWriteDid(ConnectionId connection, const Request &req) {
    auto request = uwb::protocol::decodeWriteDidRequest(req.pdu);
    if (!request.ok()) {
        respondNegative(connection, req, nrcForDecodeError(request.code()));
        return;
    }

    const std::uint16_t didValue = request.value().did;
    if (!uwb::protocol::isKnownDid(didValue)) {
        respondNegative(connection, req, ServiceNrc::RequestOutOfRange); // §26/§27: NRC 0x31
        return;
    }
    const Did did = static_cast<Did>(didValue);
    const ByteBuffer record = request.value().data;

    const ConnectionContext *ctx = deps_.connections.find(connection);
    if (ctx == nullptr) {
        return;
    }

    auto outcome = deps_.dids.write(connection, *ctx, did, uwb::protocol::bytesOf(record));
    if (outcome.status == ServerStatus::NeedsBackend) {
        if (deps_.backend == nullptr) {
            respondNegative(connection, req, ServiceNrc::ConditionsNotCorrect);
            return;
        }
        Pending pending;
        pending.connection = connection;
        pending.requestSid = req.sid;
        pending.clientAddress = req.clientAddress;
        pending.transactionId = req.transactionId;
        pending.did = didValue;
        pending.deadlineUs = deps_.clock.monotonicUs() + p2StarUs(deps_.config);
        const std::uint32_t token = addPending(pending);

        auto submitted = deps_.backend->writeDid(
            connection, did, uwb::protocol::bytesOf(record),
            [this, token](DidWriteResult done) { completeDidWrite(token, done); });
        if (submitted.ok()) {
            setPendingOperation(token, submitted.value());
        }
        if (!submitted.ok()) {
            removePending(token);
            respondNegative(connection, req, deps_.backend->busy() ? ServiceNrc::BusyRepeatRequest
                                                                   : ServiceNrc::ConditionsNotCorrect);
            return;
        }
        respondPending(connection, req);
        return;
    }

    if (!outcome.ok()) {
        respondStatus(connection, req, outcome.status);
        return;
    }

    const WriteDidResponse response{didValue};
    const ByteBuffer pdu = uwb::protocol::encodeWriteDidResponse(response);
    respondPdu(connection, req.clientAddress, req.transactionId, uwb::protocol::bytesOf(pdu), TxPriority::High);
}

// ---------------------------------------------------------------------------
// 0x31 Routine Control (specification §28, §42)
// ---------------------------------------------------------------------------

void ServiceDispatcher::handleRoutineControl(ConnectionId connection, const Request &req) {
    auto request = uwb::protocol::decodeRoutineControlRequest(req.pdu);
    if (!request.ok()) {
        respondNegative(connection, req, nrcForDecodeError(request.code()));
        return;
    }

    const RoutineControlRequest &control = request.value();
    if (!uwb::protocol::isKnownRoutine(control.routineId)) {
        respondNegative(connection, req, ServiceNrc::RequestOutOfRange); // §21.4
        return;
    }

    const ConnectionContext *ctx = deps_.connections.find(connection);
    if (ctx == nullptr) {
        return;
    }

    const auto controlType = static_cast<RoutineControlType>(control.controlType);
    const RoutineId routine = static_cast<RoutineId>(control.routineId);
    const bool stateChanging = controlType == RoutineControlType::Start || controlType == RoutineControlType::Stop;

    if (stateChanging && !ctx->isControl()) {
        respondNegative(connection, req, ServiceNrc::ConditionsNotCorrect); // §9.2
        return;
    }
    if (controlType == RoutineControlType::Start) {
        if (deps_.routines.find(routine, connection) != nullptr) {
            respondNegative(connection, req, ServiceNrc::ConditionsNotCorrect); // §28: already running
            return;
        }
        ConstBytes options =
            control.optionRecord.empty() ? ConstBytes{} : uwb::protocol::bytesOf(control.optionRecord);
        auto started = deps_.routines.start(routine, connection, options);
        if (!started.ok()) {
            respondStatus(connection, req, started.status);
            return;
        }

        const RoutineService::Routine running = started.value;
        if (running.needsBackend) {
            if (deps_.backend == nullptr) {
                deps_.routines.noteSubmitFailed(running.handle);
                respondNegative(connection, req, ServiceNrc::ConditionsNotCorrect);
                return;
            }

            Pending pending;
            pending.connection = connection;
            pending.requestSid = req.sid;
            pending.clientAddress = req.clientAddress;
            pending.transactionId = req.transactionId;
            pending.routineId = control.routineId;
            pending.controlType = control.controlType;
            pending.deadlineUs = deps_.clock.monotonicUs() + p2StarUs(deps_.config);
            const std::uint32_t token = addPending(pending);

            auto submitted = deps_.backend->startRoutine(connection, routine, options,
                                                         [this, token](RoutineResult done) {
                                                             completeRoutine(token, done);
                                                         });
            if (!submitted.ok()) {
                removePending(token);
                deps_.routines.noteSubmitFailed(running.handle);
                respondNegative(connection, req, deps_.backend->busy() ? ServiceNrc::BusyRepeatRequest
                                                                      : ServiceNrc::ConditionsNotCorrect);
                return;
            }

            deps_.routines.noteBackendOperation(running.handle, submitted.value());
            setPendingOperation(token, submitted.value());
            respondPending(connection, req); // §21.4 NRC 0x78 until the routine finishes
            return;
        }

        const auto effect = deps_.routines.takeEffect(routine);
        if (effect.reinitBackend) {
            hostAction_.reinitBackend = true; // §42: server-local effect of 0x0200
        }
        const RoutineControlResponse response{control.controlType, control.routineId,
                                              static_cast<std::uint8_t>(RoutineState::Completed), {}};
        const ByteBuffer pdu = uwb::protocol::encodeRoutineControlResponse(response);
        respondPdu(connection, req.clientAddress, req.transactionId, uwb::protocol::bytesOf(pdu), TxPriority::High);
        return;
    }

    if (controlType == RoutineControlType::Stop) {
        auto stopped = deps_.routines.stop(routine, connection, deps_.backend);
        if (!stopped.ok()) {
            respondStatus(connection, req, stopped.status);
            return;
        }
        const RoutineControlResponse response{control.controlType, control.routineId,
                                              static_cast<std::uint8_t>(stopped.value.state), {}};
        const ByteBuffer pdu = uwb::protocol::encodeRoutineControlResponse(response);
        respondPdu(connection, req.clientAddress, req.transactionId, uwb::protocol::bytesOf(pdu), TxPriority::High);
        return;
    }

    auto results = deps_.routines.requestResults(routine, connection, deps_.backend);
    if (!results.ok()) {
        respondStatus(connection, req, results.status);
        return;
    }
    const RoutineControlResponse response{control.controlType, control.routineId,
                                          static_cast<std::uint8_t>(results.value.state),
                                          results.value.statusRecord};
    const ByteBuffer pdu = uwb::protocol::encodeRoutineControlResponse(response);
    respondPdu(connection, req.clientAddress, req.transactionId, uwb::protocol::bytesOf(pdu), TxPriority::High);
}

// ---------------------------------------------------------------------------
// 0x3E Client Present (specification §29)
// ---------------------------------------------------------------------------

void ServiceDispatcher::handleClientPresent(ConnectionId connection, const Request &req) {
    auto request = uwb::protocol::decodeClientPresentRequest(req.pdu);
    if (!request.ok()) {
        respondNegative(connection, req, ServiceNrc::IncorrectMessageLengthOrInvalidFormat);
        return;
    }
    deps_.connections.touch(connection, deps_.clock.monotonicUs());
    const ByteBuffer pdu = uwb::protocol::encodeClientPresentResponse();
    respondPdu(connection, req.clientAddress, req.transactionId, uwb::protocol::bytesOf(pdu), TxPriority::High);
}

// ---------------------------------------------------------------------------
// 0x40 Execute AT Command (specification §30)
// ---------------------------------------------------------------------------

void ServiceDispatcher::handleAtCommand(ConnectionId connection, const Request &req) {
    if (!deps_.config.rawAtEnabled || deps_.backend == nullptr) {
        // §25: raw AT is capability- and configuration-gated.
        respondNegative(connection, req, ServiceNrc::ServiceNotSupportedInActiveSession);
        return;
    }

    const ConnectionContext *ctx = deps_.connections.find(connection);
    if (ctx == nullptr) {
        return;
    }
    if (!ctx->isControl() || !ctx->isExtendedSession()) {
        respondNegative(connection, req, ServiceNrc::ConditionsNotCorrect); // §30
        return;
    }
    if (deps_.config.securityEnabled && !ctx->isUnlocked()) {
        respondNegative(connection, req, ServiceNrc::SecurityAccessDenied); // §30
        return;
    }

    auto request = uwb::protocol::decodeExecuteAtRequest(req.pdu);
    if (!request.ok()) {
        respondNegative(connection, req, nrcForDecodeError(request.code())); // §30: oversized AT is 0x31
        return;
    }

    Pending pending;
    pending.connection = connection;
    pending.requestSid = req.sid;
    pending.clientAddress = req.clientAddress;
    pending.transactionId = req.transactionId;
    pending.deadlineUs = deps_.clock.monotonicUs() + p2StarUs(deps_.config);
    const std::uint32_t token = addPending(pending);

    auto submitted = deps_.backend->submitAt(connection, request.value().command,
                                             deps_.config.timeouts.uartCommandTimeoutMs,
                                             [this, token](AtResult done) { completeAt(token, done); });
    if (submitted.ok()) {
        setPendingOperation(token, submitted.value());
    }
    if (!submitted.ok()) {
        removePending(token);
        respondNegative(connection, req, deps_.backend->busy() ? ServiceNrc::BusyRepeatRequest
                                                               : ServiceNrc::ConditionsNotCorrect);
        return;
    }
    respondPending(connection, req);
}

// ---------------------------------------------------------------------------
// 0x41 Event Control (specification §31)
// ---------------------------------------------------------------------------

void ServiceDispatcher::handleEventControl(ConnectionId connection, const Request &req) {
    if (req.pdu.size() < 2) {
        respondNegative(connection, req, ServiceNrc::IncorrectMessageLengthOrInvalidFormat);
        return;
    }
    const auto subFunction = static_cast<uwb::protocol::EventControlSubFunction>(req.pdu[1]);

    switch (subFunction) {
    case uwb::protocol::EventControlSubFunction::Subscribe: {
        auto request = uwb::protocol::decodeEventSubscribeRequest(req.pdu);
        if (!request.ok()) {
            respondNegative(connection, req, nrcForDecodeError(request.code()));
            return;
        }
        const EventSubscribeRequest &sub = request.value();
        auto stream = deps_.events.subscribe(connection, static_cast<uwb::protocol::EventId>(sub.eventId),
                                             static_cast<uwb::protocol::StreamMode>(sub.mode), sub.requestedPeriodMs);
        if (!stream.ok()) {
            respondStatus(connection, req, stream.status);
            return;
        }
        EventSubscribeResponse response;
        response.eventId = static_cast<std::uint16_t>(stream.value.eventId);
        response.streamId = stream.value.streamId;
        response.acceptedMode = static_cast<std::uint8_t>(stream.value.mode);
        response.flags = 0;
        response.acceptedPeriodMs = stream.value.periodMs;
        response.queueCapacity = static_cast<std::uint16_t>(deps_.events.queueCapacity(stream.value.mode));
        const ByteBuffer pdu = uwb::protocol::encodeEventSubscribeResponse(response);
        respondPdu(connection, req.clientAddress, req.transactionId, uwb::protocol::bytesOf(pdu), TxPriority::High);
        return;
    }
    case uwb::protocol::EventControlSubFunction::Unsubscribe: {
        auto request = uwb::protocol::decodeEventUnsubscribeRequest(req.pdu);
        if (!request.ok()) {
            respondNegative(connection, req, ServiceNrc::IncorrectMessageLengthOrInvalidFormat);
            return;
        }
        auto removed = deps_.events.unsubscribe(connection, request.value().streamId);
        if (!removed.ok()) {
            respondNegative(connection, req, ServiceNrc::RequestOutOfRange); // unknown stream id
            return;
        }
        const ByteBuffer pdu = uwb::protocol::encodeEventUnsubscribeResponse(
            EventUnsubscribeResponse{removed.value.streamId});
        respondPdu(connection, req.clientAddress, req.transactionId, uwb::protocol::bytesOf(pdu), TxPriority::High);
        return;
    }
    case uwb::protocol::EventControlSubFunction::QuerySubscription: {
        auto request = uwb::protocol::decodeEventQueryRequest(req.pdu);
        if (!request.ok()) {
            respondNegative(connection, req, ServiceNrc::IncorrectMessageLengthOrInvalidFormat);
            return;
        }
        auto info = deps_.events.query(connection, request.value().streamId);
        if (!info.ok()) {
            respondNegative(connection, req, ServiceNrc::RequestOutOfRange);
            return;
        }
        EventQueryResponse response;
        response.streamId = info.value.streamId;
        response.eventId = static_cast<std::uint16_t>(info.value.eventId);
        response.mode = static_cast<std::uint8_t>(info.value.mode);
        response.state = static_cast<std::uint8_t>(info.value.state);
        response.periodMs = info.value.periodMs;
        response.droppedCount = info.value.droppedCount;
        const ByteBuffer pdu = uwb::protocol::encodeEventQueryResponse(response);
        respondPdu(connection, req.clientAddress, req.transactionId, uwb::protocol::bytesOf(pdu), TxPriority::High);
        return;
    }
    case uwb::protocol::EventControlSubFunction::UnsubscribeAll: {
        auto request = uwb::protocol::decodeEventUnsubscribeAllRequest(req.pdu);
        if (!request.ok()) {
            respondNegative(connection, req, ServiceNrc::IncorrectMessageLengthOrInvalidFormat);
            return;
        }
        std::ignore = deps_.events.unsubscribeAll(connection);
        const ByteBuffer pdu = uwb::protocol::encodeEventUnsubscribeAllResponse();
        respondPdu(connection, req.clientAddress, req.transactionId, uwb::protocol::bytesOf(pdu), TxPriority::High);
        return;
    }
    default:
        respondNegative(connection, req, ServiceNrc::SubFunctionNotSupported); // §21.4
        return;
    }
}

// ---------------------------------------------------------------------------
// Asynchronous request bookkeeping (§21.4 NRC 0x78 -> final response)
// ---------------------------------------------------------------------------

std::uint32_t ServiceDispatcher::addPending(Pending pending) {
    pending.token = nextToken_++;
    pending_.push_back(pending);
    return pending.token;
}

void ServiceDispatcher::setPendingOperation(std::uint32_t token, OperationId operation) noexcept {
    for (Pending &p : pending_) {
        if (p.token == token) {
            p.operation = operation;
            return;
        }
    }
}

void ServiceDispatcher::removePending(std::uint32_t token) noexcept {
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [&](const Pending &p) { return p.token == token; }),
                   pending_.end());
}

ServiceDispatcher::Request ServiceDispatcher::requestOf(const Pending &pending) const noexcept {
    Request req;
    req.sid = pending.requestSid;
    req.clientAddress = pending.clientAddress;
    req.transactionId = pending.transactionId;
    return req;
}

std::size_t ServiceDispatcher::pendingCount(ConnectionId connection) const noexcept {
    std::size_t count = 0;
    for (const Pending &p : pending_) {
        if (p.connection == connection) {
            ++count;
        }
    }
    return count;
}

void ServiceDispatcher::connectionClosed(ConnectionId connection) {
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [&](const Pending &p) { return p.connection == connection; }),
                   pending_.end());
    if (deps_.backend != nullptr) {
        deps_.backend->cancelOperations(connection); // §9.4
    }
    deps_.routines.stopAllForConnection(connection);
    deps_.events.connectionClosed(connection);
    if (deps_.security != nullptr) {
        deps_.security->reset(connection);
    }
}

void ServiceDispatcher::tick(std::uint64_t nowUs) {
    // Hard backstop for asynchronous requests: if the backend misses P2*, the
    // client must still receive a final response (§23, §52).
    std::vector<Pending> expired;
    for (const Pending &p : pending_) {
        if (p.deadlineUs != 0 && nowUs >= p.deadlineUs) {
            expired.push_back(p);
        }
    }
    for (const Pending &p : expired) {
        if (p.operation != kInvalidOperationId && deps_.backend != nullptr) {
            deps_.backend->requestRoutineCancel(p.operation);
            deps_.routines.completeBackendOperation(p.operation, BackendStatus::Timeout, {});
        }
        const Request req = requestOf(p);
        respondNegative(p.connection, req, ServiceNrc::GeneralProgrammingFailure); // §21.4
        removePending(p.token);
    }
}

ServiceDispatcher::HostAction ServiceDispatcher::takeHostAction() noexcept {
    HostAction action = hostAction_;
    hostAction_ = {};
    return action;
}

void ServiceDispatcher::completeAt(std::uint32_t token, AtResult result) {
    auto it = std::find_if(pending_.begin(), pending_.end(), [&](const Pending &p) { return p.token == token; });
    if (it == pending_.end()) {
        return;
    }
    const Pending pending = *it;
    removePending(token);

    const Request req = requestOf(pending);
    if (result.status != BackendStatus::Completed) {
        respondStatus(pending.connection, req, fromBackendStatus(result.status));
        return;
    }
    const ByteBuffer pdu = uwb::protocol::encodeExecuteAtResponse(
        uwb::protocol::ExecuteAtResponse{std::move(result.response)});
    respondPdu(pending.connection, req.clientAddress, req.transactionId, uwb::protocol::bytesOf(pdu),
               TxPriority::High); // §38 / SRV-005: service responses outrank streaming
}

void ServiceDispatcher::completeDidRead(std::uint32_t token, DidReadResult result) {
    auto it = std::find_if(pending_.begin(), pending_.end(), [&](const Pending &p) { return p.token == token; });
    if (it == pending_.end()) {
        return;
    }
    const Pending pending = *it;
    removePending(token);

    const Request req = requestOf(pending);
    if (result.status != BackendStatus::Completed) {
        respondStatus(pending.connection, req, fromBackendStatus(result.status));
        return;
    }
    const ReadDidResponse response{pending.did, std::move(result.record)};
    const ByteBuffer pdu = uwb::protocol::encodeReadDidResponse(response);
    respondPdu(pending.connection, req.clientAddress, req.transactionId, uwb::protocol::bytesOf(pdu),
               TxPriority::High);
}

void ServiceDispatcher::completeDidWrite(std::uint32_t token, DidWriteResult result) {
    auto it = std::find_if(pending_.begin(), pending_.end(), [&](const Pending &p) { return p.token == token; });
    if (it == pending_.end()) {
        return;
    }
    const Pending pending = *it;
    removePending(token);

    const Request req = requestOf(pending);
    if (result.status != BackendStatus::Completed) {
        respondStatus(pending.connection, req, fromBackendStatus(result.status));
        return;
    }
    const ByteBuffer pdu = uwb::protocol::encodeWriteDidResponse(WriteDidResponse{pending.did});
    respondPdu(pending.connection, req.clientAddress, req.transactionId, uwb::protocol::bytesOf(pdu),
               TxPriority::High);
}

void ServiceDispatcher::completeRoutine(std::uint32_t token, RoutineResult result) {
    auto it = std::find_if(pending_.begin(), pending_.end(), [&](const Pending &p) { return p.token == token; });
    if (it == pending_.end()) {
        return;
    }
    const Pending pending = *it;
    removePending(token);

    const Request req = requestOf(pending);
    if (pending.operation != kInvalidOperationId) {
        deps_.routines.completeBackendOperation(pending.operation, result.status,
                                                uwb::protocol::bytesOf(result.statusRecord));
    }
    const RoutineState state = result.status == BackendStatus::Completed
                                   ? RoutineState::Completed
                                   : (result.status == BackendStatus::Cancelled ? RoutineState::Cancelled
                                                                                : RoutineState::Failed);
    const RoutineControlResponse response{pending.controlType, pending.routineId, static_cast<std::uint8_t>(state),
                                          std::move(result.statusRecord)};
    const ByteBuffer pdu = uwb::protocol::encodeRoutineControlResponse(response);
    respondPdu(pending.connection, req.clientAddress, req.transactionId, uwb::protocol::bytesOf(pdu),
               TxPriority::High);
}

} // namespace uwb::server
