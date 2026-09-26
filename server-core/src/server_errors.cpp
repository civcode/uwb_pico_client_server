#include "uwb/server/server_errors.hpp"

#include "uwb/server/security_provider.hpp"
#include "uwb/server/uwb_backend.hpp"

namespace uwb::server {

using uwb::protocol::ServiceNrc;

// Server outcome -> §21.4 NRC mapping. Keeping this in one table is what allows
// services to stay wire-format agnostic.
std::optional<ServiceNrc> nrcFor(ServerStatus status) noexcept {
    switch (status) {
    case ServerStatus::Ok:
        return std::nullopt;

    // A request that arrives before activation or out of order (§9.1).
    case ServerStatus::NotActivated:
        return ServiceNrc::RequestSequenceError; // 0x24

    // Role/session policy denies the request (§9.2, §22.3, §23).
    case ServerStatus::RoleDenied:
    case ServerStatus::SessionDenied:
    case ServerStatus::StorageUnavailable:
        return ServiceNrc::ConditionsNotCorrect; // 0x22

    case ServerStatus::SecurityDenied:
        return ServiceNrc::SecurityAccessDenied; // 0x33

    case ServerStatus::InvalidAddress:
        return ServiceNrc::RequestOutOfRange; // 0x31

    // Event service (§31).
    case ServerStatus::UnknownStream:
    case ServerStatus::UnknownEvent:
        return ServiceNrc::RequestOutOfRange; // 0x31
    case ServerStatus::TooManyStreams:
        return ServiceNrc::ConditionsNotCorrect; // 0x22

    // DID service (§26, §27, §39).
    case ServerStatus::UnknownDid:
    case ServerStatus::UnsupportedDid:
        return ServiceNrc::RequestOutOfRange; // 0x31
    case ServerStatus::ReadOnlyDid:
        return ServiceNrc::ConditionsNotCorrect; // 0x22
    case ServerStatus::InvalidRecord:
        return ServiceNrc::IncorrectMessageLengthOrInvalidFormat; // 0x13

    // Routine service (§28, §42).
    case ServerStatus::UnknownRoutine:
        return ServiceNrc::RequestOutOfRange; // 0x31
    case ServerStatus::RoutineNotRunning:
    case ServerStatus::RoutineAlreadyRunning:
        return ServiceNrc::RequestSequenceError; // 0x24
    case ServerStatus::RoutineNotCancellable:
        return ServiceNrc::ConditionsNotCorrect; // 0x22

    // Backend / AT service.
    case ServerStatus::NeedsBackend:
        return std::nullopt; // asynchronous path, decided by the dispatcher
    case ServerStatus::BackendBusy:
        return ServiceNrc::BusyRepeatRequest; // 0x21
    case ServerStatus::BackendTimeout:
    case ServerStatus::BackendFailed:
        return ServiceNrc::GeneralProgrammingFailure; // 0x72
    case ServerStatus::AtCommandTooLong:
        return ServiceNrc::RequestOutOfRange; // 0x31
    case ServerStatus::AtDisabled:
        return ServiceNrc::ServiceNotSupported; // 0x11

    case ServerStatus::Generic:
        return ServiceNrc::GeneralReject; // 0x10
    }
    return ServiceNrc::GeneralReject;
}

ServiceNrc nrcForSecurity(SecurityDecision decision) noexcept {
    switch (decision) {
    case SecurityDecision::Accepted:
        return ServiceNrc::GeneralReject; // not used: accepted is not a negative response
    case SecurityDecision::BadKey:
        return ServiceNrc::InvalidKey; // 0x35 (§21.4)
    case SecurityDecision::Denied:
        return ServiceNrc::SecurityAccessDenied; // 0x33
    case SecurityDecision::TooManyAttempts:
        return ServiceNrc::ExceedNumberOfAttempts; // 0x36
    case SecurityDecision::DelayNotExpired:
        return ServiceNrc::RequiredTimeDelayNotExpired; // 0x37
    case SecurityDecision::Failed:
        return ServiceNrc::GeneralReject; // 0x10
    }
    return ServiceNrc::GeneralReject;
}

ServerStatus fromBackendStatus(BackendStatus status) noexcept {
    switch (status) {
    case BackendStatus::Completed:
        return ServerStatus::Ok;
    case BackendStatus::Timeout:
        return ServerStatus::BackendTimeout;
    case BackendStatus::ParseError:
        return ServerStatus::BackendFailed;
    case BackendStatus::Busy:
        return ServerStatus::BackendBusy;
    case BackendStatus::Cancelled:
        return ServerStatus::Generic;
    case BackendStatus::Unsupported:
        return ServerStatus::UnsupportedDid;
    case BackendStatus::Failed:
        return ServerStatus::BackendFailed;
    }
    return ServerStatus::Generic;
}

} // namespace uwb::server
