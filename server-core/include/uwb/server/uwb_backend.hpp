#pragma once

#include <cstdint>
#include <functional>

#include "uwb/protocol/bytes.hpp"
#include "uwb/protocol/dids.hpp"
#include "uwb/protocol/result.hpp"
#include "uwb/protocol/routines.hpp"

namespace uwb::server {

using uwb::protocol::ConstBytes;
using uwb::protocol::Did;
using uwb::protocol::Result;
using uwb::protocol::RoutineId;

// Result of one asynchronous backend operation.
enum class BackendStatus : std::uint8_t {
    Completed = 0,
    Timeout = 1,       // UART command timeout (specification §48.1, §52)
    ParseError = 2,    // module answered something unexpected (§48.1)
    Busy = 3,          // UART semantic channel occupied (§48.2)
    Cancelled = 4,
    Unsupported = 5,   // operation not available on this backend/module
    Failed = 6,
};

// Opaque id of an in-flight backend operation.
using OperationId = std::uint32_t;
inline constexpr OperationId kInvalidOperationId = 0;

struct AtResult {
    BackendStatus status = BackendStatus::Completed;
    uwb::protocol::ByteBuffer response; // raw module text, CRLF preserved
};

struct DidReadResult {
    BackendStatus status = BackendStatus::Completed;
    uwb::protocol::ByteBuffer record; // §40 record bytes
};

struct DidWriteResult {
    BackendStatus status = BackendStatus::Completed;
};

struct RoutineResult {
    BackendStatus status = BackendStatus::Completed;
    uwb::protocol::ByteBuffer statusRecord; // §28 statusRecord
};

using AtCompletion = std::function<void(AtResult)>;
using DidReadCompletion = std::function<void(DidReadResult)>;
using DidWriteCompletion = std::function<void(DidWriteResult)>;
using RoutineCompletion = std::function<void(RoutineResult)>;

// The UWB backend owns the UWB semantic channel exclusively (specification §48).
// Service handlers submit work here and are completed asynchronously: the server
// core never blocks waiting for a module answer (implementation plan §18).
//
// Only one solicited AT exchange may be in flight at a time (§48.2); the backend
// enforces that plus the inter-command gap and its own bounded command queue
// (§53). Completions are delivered from tick(), never from another thread inside
// the core.
class IUwbBackend {
public:
    virtual ~IUwbBackend() = default;

    // "owner" is the ConnectionId that submitted the operation, so a disconnect
    // can cancel exactly that connection's pending work (specification §9.4).

    // Raw AT text without CR/LF; the backend appends the module terminator.
    [[nodiscard]] virtual Result<OperationId> submitAt(std::uint32_t owner, ConstBytes command,
                                                        std::uint32_t timeoutMs, AtCompletion done) = 0;

    [[nodiscard]] virtual Result<OperationId> readDid(std::uint32_t owner, Did did,
                                                       DidReadCompletion done) = 0;
    [[nodiscard]] virtual Result<OperationId> writeDid(std::uint32_t owner, Did did, ConstBytes record,
                                                        DidWriteCompletion done) = 0;

    // Routine execution (§28, §42). optionRecord may be empty.
    [[nodiscard]] virtual Result<OperationId> startRoutine(std::uint32_t owner, RoutineId routine,
                                                            ConstBytes optionRecord,
                                                            RoutineCompletion done) = 0;

    // Ask a running routine to cancel at its next safe point. Returns false when
    // the routine is not cancellable (§28).
    [[nodiscard]] virtual bool requestRoutineCancel(OperationId operation) noexcept = 0;

    // Drop pending operations owned by a connection that went away (§9.4).
    virtual void cancelOperations(std::uint32_t owner) noexcept = 0;

    // Drive UART timeouts, inter-command gaps, and completion callbacks.
    virtual void tick(std::uint64_t nowUs) noexcept = 0;

    [[nodiscard]] virtual bool busy() const noexcept = 0;
    [[nodiscard]] virtual std::size_t pendingOperations() const noexcept = 0;

};

} // namespace uwb::server
