#pragma once

#include <cstdint>
#include <vector>

#include "uwb/protocol/result.hpp"
#include "uwb/protocol/routines.hpp"
#include "uwb/server/server_errors.hpp"
#include "uwb/server/transport.hpp"
#include "uwb/server/uwb_backend.hpp"

namespace uwb::server {

using uwb::protocol::ByteBuffer;
using uwb::protocol::ConstBytes;
using uwb::protocol::RoutineId;
using uwb::protocol::RoutineState;

// Routine lifecycle (specification §42, implementation plan §16.6).
//
//   Idle -> Running -> (Stopping) -> Completed | Failed | Cancelled
//
// Every request carries an owner connection; when that connection disconnects all
// of its routines are stopped (specification §9.4).
class RoutineService {
public:
    struct Routine {
        std::uint32_t handle = 0; // server-local handle, unique while running
        RoutineId routineId = RoutineId::InitializeUwbBackend;
        RoutineState state = RoutineState::Idle;
        ConnectionId owner = kInvalidConnectionId;
        OperationId backendOperation = kInvalidOperationId;
        bool cancellable = true; // §42 policy
        bool cancelRequested = false;
        bool needsBackend = false; // false for server-core-local routines
        ByteBuffer statusRecord;
    };

    RoutineService() = default;

    // Policy query (specification §42): "Usually no" routines are treated as not
    // cancellable while their critical section runs.
    [[nodiscard]] static bool routineCancellable(RoutineId routine) noexcept;

    // Local (server-core implemented) routine: 0x0200 Initialize UWB Backend.
    // Every other v1 routine is delegated to IUwbBackend.
    [[nodiscard]] static bool routineIsLocal(RoutineId routine) noexcept;

    // Validation + lifecycle bookkeeping only. When the returned Routine has
    // needsBackend set, the caller (the dispatcher) submits the operation to
    // IUwbBackend with its own completion callback so the client receives the
    // 0x78 response-pending and the final response (§21.4, §28).
    [[nodiscard]] ServerResult<Routine> start(RoutineId routine, ConnectionId owner, ConstBytes options);

    void noteBackendOperation(std::uint32_t handle, OperationId operation) noexcept;
    void noteSubmitFailed(std::uint32_t handle) noexcept;
    [[nodiscard]] ServerResult<Routine> requestResults(RoutineId routine, ConnectionId owner, IUwbBackend *backend);
    [[nodiscard]] ServerResult<Routine> stop(RoutineId routine, ConnectionId owner, IUwbBackend *backend);

    // Called when the backend finishes a routine operation.
    void completeBackendOperation(OperationId operation, BackendStatus status, ConstBytes statusRecord);

    // Cancel at the next safe point; false for non-cancellable routines (§28).
    [[nodiscard]] bool requestCancel(RoutineId routine, ConnectionId owner);

    // §9.4: everything owned by a closed connection is stopped.
    std::size_t stopAllForConnection(ConnectionId owner);

    [[nodiscard]] const Routine *find(RoutineId routine, ConnectionId owner) const noexcept;
    [[nodiscard]] const Routine *findByOperation(OperationId operation) const noexcept;
    [[nodiscard]] std::size_t runningCount() const noexcept;
    [[nodiscard]] std::size_t count() const noexcept { return routines_.size(); }

    // Local routine effects are reported to the server core through this return
    // value so RoutineService never reaches into connections or streams.
    struct LocalEffect {
        bool reinitBackend = false; // 0x0200 reinitialize backend/parser state
    };
    [[nodiscard]] LocalEffect takeEffect(RoutineId routine) noexcept;

private:
    [[nodiscard]] Routine *mutableFind(RoutineId routine, ConnectionId owner) noexcept;

    std::vector<Routine> routines_;
    std::uint32_t nextHandle_ = 1;
};

} // namespace uwb::server
