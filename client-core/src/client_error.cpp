#include "uwb/client/client_error.hpp"

#include <cstdio>

namespace uwb::client {

const char *errorDomainName(ErrorDomain domain) noexcept {
    switch (domain) {
    case ErrorDomain::None: return "none";
    case ErrorDomain::Network: return "network";
    case ErrorDomain::Transport: return "transport";
    case ErrorDomain::Protocol: return "protocol";
    case ErrorDomain::Service: return "service";
    case ErrorDomain::Device: return "device";
    case ErrorDomain::Uwb: return "uwb";
    case ErrorDomain::Configuration: return "configuration";
    case ErrorDomain::Storage: return "storage";
    case ErrorDomain::Stream: return "stream";
    case ErrorDomain::Calibration: return "calibration";
    case ErrorDomain::Simulation: return "simulation";
    case ErrorDomain::Application: return "application";
    }
    return "unknown";
}

const char *clientErrorCodeName(ClientErrorCode code) noexcept {
    switch (code) {
    case ClientErrorCode::None: return "None";
    case ClientErrorCode::Timeout: return "Timeout";
    case ClientErrorCode::Disconnected: return "Disconnected";
    case ClientErrorCode::NotConnected: return "NotConnected";
    case ClientErrorCode::NotReady: return "NotReady";
    case ClientErrorCode::ActivationRejected: return "ActivationRejected";
    case ClientErrorCode::FramingError: return "FramingError";
    case ClientErrorCode::DecodeError: return "DecodeError";
    case ClientErrorCode::EncodeError: return "EncodeError";
    case ClientErrorCode::ServiceRejected: return "ServiceRejected";
    case ClientErrorCode::TooManyOutstanding: return "TooManyOutstanding";
    case ClientErrorCode::Cancelled: return "Cancelled";
    case ClientErrorCode::DeviceNotFound: return "DeviceNotFound";
    case ClientErrorCode::InvalidArgument: return "InvalidArgument";
    case ClientErrorCode::InvalidState: return "InvalidState";
    case ClientErrorCode::QueueOverflow: return "QueueOverflow";
    case ClientErrorCode::ConnectFailed: return "ConnectFailed";
    case ClientErrorCode::ConnectTimeout: return "ConnectTimeout";
    case ClientErrorCode::TransportFailure: return "TransportFailure";
    case ClientErrorCode::DeviceBusy: return "DeviceBusy";
    case ClientErrorCode::StorageFailed: return "StorageFailed";
    case ClientErrorCode::ProtocolViolation: return "ProtocolViolation";
    }
    return "Unknown";
}

ClientError makeError(ErrorDomain domain, ClientErrorCode code, std::string message) noexcept {
    ClientError error;
    error.domain = domain;
    error.code = static_cast<int>(code);
    error.message = std::move(message);
    return error;
}

std::string ClientError::toString() const {
    std::string text = std::string(errorDomainName(domain)) + ": " +
                       clientErrorCodeName(static_cast<ClientErrorCode>(code));
    if (!message.empty()) {
        text += ": " + message;
    }
    if (transactionId.has_value()) {
        char buffer[48];
        std::snprintf(buffer, sizeof(buffer), " [transaction 0x%08llx", static_cast<unsigned long long>(*transactionId));
        text += buffer;
        if (serviceId.has_value()) {
            char extra[16];
            std::snprintf(extra, sizeof(extra), ", sid 0x%02x", static_cast<unsigned>(*serviceId));
            text += extra;
        }
        if (nrc.has_value()) {
            char extra[16];
            std::snprintf(extra, sizeof(extra), ", nrc 0x%02x", static_cast<unsigned>(*nrc));
            text += extra;
        }
        text += "]";
    }
    return text;
}

} // namespace uwb::client
