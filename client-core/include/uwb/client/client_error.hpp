#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "uwb/domain/device_identity.hpp"

namespace uwb::client {

// Client error model (specification §69). Low-level failures are translated
// into this hierarchy while the raw codes stay available for diagnostics.
enum class ErrorDomain : std::uint8_t {
    None = 0,
    Network = 1,
    Transport = 2,
    Protocol = 3,
    Service = 4,
    Device = 5,
    Uwb = 6,
    Configuration = 7,
    Storage = 8,
    Stream = 9,
    Calibration = 10,
    Simulation = 11,
    Application = 12,
};

[[nodiscard]] const char *errorDomainName(ErrorDomain domain) noexcept;

// Stable client-level error codes. Service rejections keep the raw NRC in
// ClientError::nrc; this code only says what kind of failure happened.
enum class ClientErrorCode : int {
    None = 0,
    Timeout = 1,
    Disconnected = 2,
    NotConnected = 3,
    NotReady = 4,
    ActivationRejected = 5,
    FramingError = 6,
    DecodeError = 7,
    EncodeError = 8,
    ServiceRejected = 9,
    TooManyOutstanding = 10,
    Cancelled = 11,
    DeviceNotFound = 12,
    InvalidArgument = 13,
    InvalidState = 14,
    QueueOverflow = 15,
    ConnectFailed = 16,
    ConnectTimeout = 17,
    TransportFailure = 18,
    DeviceBusy = 19,
    StorageFailed = 20,
    ProtocolViolation = 21,
};

[[nodiscard]] const char *clientErrorCodeName(ClientErrorCode code) noexcept;

struct ClientError {
    ErrorDomain domain = ErrorDomain::Network;
    int code = static_cast<int>(ClientErrorCode::None);
    std::string message;

    std::optional<domain::DeviceUuid> device;
    std::optional<std::uint32_t> transactionId;
    std::optional<std::uint8_t> serviceId;
    std::optional<std::uint8_t> nrc;

    [[nodiscard]] std::string toString() const;
};

[[nodiscard]] ClientError makeError(ErrorDomain domain, ClientErrorCode code, std::string message) noexcept;

// Result carrier for the client layer; mirrors uwb::protocol::Result but
// carries the richer §69 error object.
template <typename T>
class [[nodiscard]] ClientResult {
public:
    static ClientResult ok(T value) { return ClientResult{std::move(value), std::nullopt}; }

    static ClientResult error(ClientError error) { return ClientResult{std::nullopt, std::move(error)}; }

    static ClientResult error(ErrorDomain domain, ClientErrorCode code, std::string message) {
        return ClientResult{std::nullopt, makeError(domain, code, std::move(message))};
    }

    [[nodiscard]] bool ok() const noexcept { return value_.has_value(); }
    [[nodiscard]] bool failed() const noexcept { return !value_.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] const T &value() const & { return *value_; }
    [[nodiscard]] T &value() & { return *value_; }
    [[nodiscard]] T &&value() && { return std::move(*value_); }

    [[nodiscard]] const T &operator*() const & { return *value_; }

    [[nodiscard]] const ClientError &error() const noexcept {
        static const ClientError none;
        return error_.has_value() ? *error_ : none;
    }
    [[nodiscard]] const std::optional<ClientError> &errorOption() const noexcept { return error_; }

private:
    ClientResult(std::optional<T> value, std::optional<ClientError> error) noexcept
        : value_(std::move(value)), error_(std::move(error)) {}

    std::optional<T> value_;
    std::optional<ClientError> error_;
};

} // namespace uwb::client
