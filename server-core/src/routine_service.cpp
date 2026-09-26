#include "uwb/server/routine_service.hpp"

#include <algorithm>

namespace uwb::server {


bool RoutineService::routineCancellable(RoutineId routine) noexcept {
    // Single source of truth: the shared routine table (specification §42).
    return uwb::protocol::routineIsCancellable(routine);
}

bool RoutineService::routineIsLocal(RoutineId routine) noexcept {
    // 0x0200 reinitializes server-side backend/parser state and needs no AT
    // exchange (specification §42).
    return routine == RoutineId::InitializeUwbBackend;
}

RoutineService::Routine *RoutineService::mutableFind(RoutineId routine, ConnectionId owner) noexcept {
    for (auto &entry : routines_) {
        if (entry.routineId == routine && entry.owner == owner) {
            return &entry;
        }
    }
    return nullptr;
}

const RoutineService::Routine *RoutineService::find(RoutineId routine, ConnectionId owner) const noexcept {
    for (const auto &entry : routines_) {
        if (entry.routineId == routine && entry.owner == owner) {
            return &entry;
        }
    }
    return nullptr;
}

const RoutineService::Routine *RoutineService::findByOperation(OperationId operation) const noexcept {
    if (operation == kInvalidOperationId) {
        return nullptr;
    }
    for (const auto &entry : routines_) {
        if (entry.backendOperation == operation) {
            return &entry;
        }
    }
    return nullptr;
}

std::size_t RoutineService::runningCount() const noexcept {
    return static_cast<std::size_t>(std::count_if(routines_.begin(), routines_.end(), [](const Routine &r) {
        return r.state == RoutineState::Running || r.state == RoutineState::Stopping;
    }));
}

ServerResult<RoutineService::Routine> RoutineService::start(RoutineId routine, ConnectionId owner,
                                                            ConstBytes options) {
    (void)options;

    if (!uwb::protocol::isKnownRoutine(routine)) {
        return ServerResult<Routine>::err(ServerStatus::UnknownRoutine); // §42 -> NRC 0x31
    }

    if (Routine *existing = mutableFind(routine, owner); existing != nullptr) {
        if (existing->state == RoutineState::Running || existing->state == RoutineState::Stopping) {
            return ServerResult<Routine>::err(ServerStatus::RoutineAlreadyRunning); // -> NRC 0x24
        }
        // A finished instance is replaced so a routine can be started again
        // (§28 StartRoutine after a previous Completed/Failed run).
        routines_.erase(std::remove_if(routines_.begin(), routines_.end(),
                                       [routine, owner](const Routine &e) {
                                           return e.routineId == routine && e.owner == owner;
                                       }),
                        routines_.end());
    }

    Routine entry;
    entry.handle = nextHandle_++;
    entry.routineId = routine;
    entry.owner = owner;
    entry.cancellable = routineCancellable(routine);
    entry.needsBackend = !routineIsLocal(routine);
    entry.state = RoutineState::Running;
    routines_.push_back(std::move(entry));
    return ServerResult<Routine>::okResult(routines_.back());
}

void RoutineService::noteBackendOperation(std::uint32_t handle, OperationId operation) noexcept {
    for (auto &entry : routines_) {
        if (entry.handle == handle) {
            entry.backendOperation = operation;
            return;
        }
    }
}

void RoutineService::noteSubmitFailed(std::uint32_t handle) noexcept {
    for (auto &entry : routines_) {
        if (entry.handle == handle) {
            entry.state = RoutineState::Failed;
            entry.backendOperation = kInvalidOperationId;
            return;
        }
    }
}

ServerResult<RoutineService::Routine> RoutineService::requestResults(RoutineId routine, ConnectionId owner,
                                                                     IUwbBackend *backend) {
    (void)backend;
    if (!uwb::protocol::isKnownRoutine(routine)) {
        return ServerResult<Routine>::err(ServerStatus::UnknownRoutine);
    }
    const Routine *entry = find(routine, owner);
    if (entry == nullptr) {
        // No instance: report Idle (§28 state table).
        Routine idle;
        idle.routineId = routine;
        idle.owner = owner;
        idle.state = RoutineState::Idle;
        idle.cancellable = routineCancellable(routine);
        return ServerResult<Routine>::okResult(idle);
    }
    return ServerResult<Routine>::okResult(*entry);
}

ServerResult<RoutineService::Routine> RoutineService::stop(RoutineId routine, ConnectionId owner,
                                                           IUwbBackend *backend) {
    if (!uwb::protocol::isKnownRoutine(routine)) {
        return ServerResult<Routine>::err(ServerStatus::UnknownRoutine);
    }

    Routine *entry = mutableFind(routine, owner);
    if (entry == nullptr || entry->state != RoutineState::Running) {
        return ServerResult<Routine>::err(ServerStatus::RoutineNotRunning); // -> NRC 0x24
    }

    // §28: stopRoutine is mandatory for cancellable routines; for the others the
    // implementation SHALL answer conditionsNotCorrect rather than pretend.
    if (!entry->cancellable) {
        return ServerResult<Routine>::err(ServerStatus::RoutineNotCancellable);
    }

    bool cancelled = false;
    if (backend != nullptr && entry->backendOperation != kInvalidOperationId) {
        cancelled = backend->requestRoutineCancel(entry->backendOperation);
    } else {
        cancelled = true; // local routine: safe to cancel immediately
    }

    if (!cancelled) {
        return ServerResult<Routine>::err(ServerStatus::RoutineNotCancellable);
    }

    entry->cancelRequested = true;
    entry->state = RoutineState::Stopping;
    return ServerResult<Routine>::okResult(*entry);
}

void RoutineService::completeBackendOperation(OperationId operation, BackendStatus status, ConstBytes statusRecord) {
    for (auto &entry : routines_) {
        if (entry.backendOperation == operation) {
            entry.state = status == BackendStatus::Completed   ? RoutineState::Completed
                          : status == BackendStatus::Cancelled ? RoutineState::Cancelled
                                                               : RoutineState::Failed;
            entry.statusRecord.assign(statusRecord.begin(), statusRecord.end());
            entry.backendOperation = kInvalidOperationId;
            return;
        }
    }
}

bool RoutineService::requestCancel(RoutineId routine, ConnectionId owner) {
    Routine *entry = mutableFind(routine, owner);
    if (entry == nullptr || entry->state != RoutineState::Running || !entry->cancellable) {
        return false;
    }
    entry->cancelRequested = true;
    entry->state = RoutineState::Stopping;
    return true;
}

std::size_t RoutineService::stopAllForConnection(ConnectionId owner) {
    std::size_t stopped = 0;
    for (auto &entry : routines_) {
        if (entry.owner != owner) {
            continue;
        }
        if (entry.state == RoutineState::Running || entry.state == RoutineState::Stopping) {
            ++stopped;
            entry.state = RoutineState::Cancelled;
        }
    }
    routines_.erase(std::remove_if(routines_.begin(), routines_.end(),
                                   [owner](const Routine &r) { return r.owner == owner; }),
                    routines_.end());
    return stopped;
}

RoutineService::LocalEffect RoutineService::takeEffect(RoutineId routine) noexcept {
    LocalEffect effect;
    if (routine == RoutineId::InitializeUwbBackend) {
        effect.reinitBackend = true;
        routines_.erase(std::remove_if(routines_.begin(), routines_.end(),
                                       [routine](const Routine &r) { return r.routineId == routine; }),
                        routines_.end());
    }
    return effect;
}

} // namespace uwb::server
