#pragma once

#include <optional>

#include "uwb/protocol/services.hpp"

#include "uwb/server/security_provider.hpp"
#include "uwb/server/uwb_backend.hpp"

namespace uwb::server {

using uwb::protocol::ServiceNrc;

// Server-core level outcomes. These are internal to the server: the dispatcher is
// the single place that maps them onto negative-response NRCs (specification
// §21.4), so services never encode wire errors themselves.
enum class ServerStatus : std::uint8_t {
    Ok = 0,

    // Connection / authorization
    NotActivated,
    RoleDenied,
    SessionDenied,
    SecurityDenied,
    InvalidAddress,

    // Event service (§31, §36, §37)
    UnknownStream,
    TooManyStreams,
    UnknownEvent,

    // DID service (§26, §27, §39, §40)
    UnknownDid,
    ReadOnlyDid,
    InvalidRecord,
    UnsupportedDid,

    // Routine service (§28, §42)
    UnknownRoutine,
    RoutineNotRunning,
    RoutineAlreadyRunning,
    RoutineNotCancellable,

    // Backend / storage
    NeedsBackend,
    BackendBusy,
    BackendTimeout,
    BackendFailed,
    StorageUnavailable,

    // AT service (§30)
    AtCommandTooLong,
    AtDisabled,

    Generic,
};

// Mapping to §21.4 NRCs. nullopt means "not a negative response situation"
// (Ok, or an asynchronous response-pending case handled by the caller).
[[nodiscard]] std::optional<ServiceNrc> nrcFor(ServerStatus status) noexcept;

// Provider-level outcomes mapped into the same table.
[[nodiscard]] ServiceNrc nrcForSecurity(SecurityDecision decision) noexcept;
[[nodiscard]] ServerStatus fromBackendStatus(BackendStatus status) noexcept;

template <typename T>
struct ServerResult {
    ServerStatus status = ServerStatus::Ok;
    T value{};

    [[nodiscard]] bool ok() const noexcept { return status == ServerStatus::Ok; }

    static ServerResult okResult(T value) {
        ServerResult r;
        r.status = ServerStatus::Ok;
        r.value = std::move(value);
        return r;
    }

    static ServerResult err(ServerStatus status) {
        ServerResult r;
        r.status = status;
        return r;
    }
};

} // namespace uwb::server
