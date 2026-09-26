#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "uwb/protocol/bytes.hpp"
#include "uwb/protocol/events.hpp"
#include "uwb/protocol/frame_parser.hpp"
#include "uwb/protocol/payloads.hpp"
#include "uwb/protocol/services.hpp"
#include "uwb/server/clock.hpp"
#include "uwb/server/config_storage.hpp"
#include "uwb/server/connection_context.hpp"
#include "uwb/server/did_service.hpp"
#include "uwb/server/event_service.hpp"
#include "uwb/server/routine_service.hpp"
#include "uwb/server/security_provider.hpp"
#include "uwb/server/server_config.hpp"
#include "uwb/server/server_errors.hpp"
#include "uwb/server/transport.hpp"
#include "uwb/server/tx_queue.hpp"
#include "uwb/server/uwb_backend.hpp"

namespace uwb::server {

using uwb::protocol::Frame;

// Per-connection TX queues owned by the server core, kept separate from
// ConnectionRegistry so the registry stays a pure policy object.
class ConnectionTxStore {
public:
    ConnectionTxStore(std::uint32_t highCapacity, std::uint32_t normalCapacity) noexcept
        : highCapacity_(highCapacity), normalCapacity_(normalCapacity) {}

    ConnectionTxQueue &getOrCreate(ConnectionId id);
    ConnectionTxQueue *find(ConnectionId id) noexcept;
    void remove(ConnectionId id) noexcept;
    void clear() noexcept { queues_.clear(); }

    template <typename Fn>
    void forEach(Fn &&fn) {
        for (auto &entry : queues_) {
            fn(entry.first, entry.second);
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return queues_.size(); }

private:
    std::unordered_map<ConnectionId, ConnectionTxQueue> queues_;
    std::uint32_t highCapacity_;
    std::uint32_t normalCapacity_;
};

// Service dispatcher (specification §44, §9.2, §20..§31).
//
// This is the single place that
//   * enforces activation, role, session, and security policy (§9.2, §22.3),
//   * maps server outcomes onto §21.4 NRCs,
//   * wraps service PDUs in the Application Message envelope (§20.1),
//   * tracks asynchronous requests (NRC 0x78 responsePending -> final response),
//   * reports host-level side effects (Pico reset, backend re-init).
class ServiceDispatcher {
public:
    struct Dependencies {
        const ServerConfig &config;
        const IClock &clock;
        ConnectionRegistry &connections;
        DidService &dids;
        RoutineService &routines;
        EventService &events;
        ConnectionTxStore &tx;
        ISecurityProvider *security = nullptr;
        IUwbBackend *backend = nullptr;
    };

    // Host-visible side effects requested by services.
    struct HostAction {
        bool resetPico = false;
        bool hardReset = false;
        std::uint64_t resetAfterUs = 0; // §24: reset runs after the response is flushed
        bool reinitBackend = false;     // Routine 0x0200 (§42)
        ConnectionId closeConnection = kInvalidConnectionId;
        bool closeRequested = false;
    };

    explicit ServiceDispatcher(Dependencies &deps) noexcept;

    // Handle one decoded frame of one connection.
    void handleFrame(ConnectionId connection, const Frame &frame);

    // Deadline supervision for asynchronous requests (p2*, P2*, specification §23, §52).
    void tick(std::uint64_t nowUs);

    // §9.4: drop everything owned by a connection that went away.
    void connectionClosed(ConnectionId connection);

    [[nodiscard]] std::size_t pendingCount() const noexcept { return pending_.size(); }
    [[nodiscard]] std::size_t pendingCount(ConnectionId connection) const noexcept;
    HostAction takeHostAction() noexcept;

private:
    // One accepted service request (service PDU including the SID byte).
    struct Request {
        std::uint8_t sid = 0;
        uwb::protocol::ConstBytes pdu; // full service PDU: SID + body
        std::uint16_t clientAddress = 0;
        std::uint32_t transactionId = 0;
    };

    // One accepted asynchronous request awaiting a backend completion.
    struct Pending {
        std::uint32_t token = 0;
        ConnectionId connection = kInvalidConnectionId;
        std::uint8_t requestSid = 0;
        std::uint16_t clientAddress = 0;
        std::uint32_t transactionId = 0;
        OperationId operation = kInvalidOperationId;
        std::uint16_t did = 0;
        std::uint16_t routineId = 0;
        std::uint8_t controlType = 0;
        std::uint64_t deadlineUs = 0;
    };

    void handleActivation(ConnectionId connection, const Frame &frame, ConnectionTxQueue &tx);
    void handleAliveCheck(ConnectionId connection, const Frame &frame, ConnectionTxQueue &tx);
    void handleApplicationMessage(ConnectionId connection, const Frame &frame, ConnectionTxQueue &tx);
    void dispatchService(ConnectionId connection, const Request &req);

    void handleSessionControl(ConnectionId, const Request &);
    void handleDeviceReset(ConnectionId, const Request &);
    void handleSecurityAccess(ConnectionId, const Request &);
    void handleReadDid(ConnectionId, const Request &);
    void handleWriteDid(ConnectionId, const Request &);
    void handleRoutineControl(ConnectionId, const Request &);
    void handleClientPresent(ConnectionId, const Request &);
    void handleAtCommand(ConnectionId, const Request &);
    void handleEventControl(ConnectionId, const Request &);

    void respondPdu(ConnectionId, std::uint16_t clientAddress, std::uint32_t transactionId,
                    uwb::protocol::ConstBytes servicePdu, TxPriority priority);
    void respondNegative(ConnectionId, const Request &, uwb::protocol::ServiceNrc);
    void respondStatus(ConnectionId, const Request &, ServerStatus);
    void respondPending(ConnectionId, const Request &);
    void enqueueAck(ConnectionId, const Request &);
    void enqueueNack(ConnectionId, const Request &, uwb::protocol::ApplicationNackCode);
    void enqueueActivationFailure(ConnectionId, const Frame &, ConnectionTxQueue &);

    std::uint32_t addPending(Pending pending);
    void removePending(std::uint32_t token) noexcept;
    void setPendingOperation(std::uint32_t token, OperationId operation) noexcept;
    Request requestOf(const Pending &pending) const noexcept;

    void completeAt(std::uint32_t token, uwb::server::AtResult result);
    void completeDidRead(std::uint32_t token, uwb::server::DidReadResult result);
    void completeDidWrite(std::uint32_t token, uwb::server::DidWriteResult result);
    void completeRoutine(std::uint32_t token, uwb::server::RoutineResult result);

    Dependencies &deps_;
    std::vector<Pending> pending_;
    std::uint32_t nextToken_ = 1;
    HostAction hostAction_;
};

} // namespace uwb::server
