#pragma once

#include <cstdint>
#include <functional>
#include <utility>

#include "uwb/client/client_error.hpp"
#include "uwb/protocol/events.hpp"
#include "uwb/protocol/services.hpp"

namespace uwb::client {

// One application-level request to send on a connection.
struct ServiceRequest {
    std::uint8_t sid = 0;
    uwb::protocol::ByteBuffer pdu; // full service PDU, including the SID byte

    // 0 selects the default timeout of the connection (longRoutineTimeoutMs is
    // applied automatically by the controller for routine operations).
    std::uint32_t timeoutMs = 0;
    bool ackRequired = false;
};

// Final response of one transaction. 0x78 ResponsePending frames are handled by
// the connection and never surface here as a final answer (specification §21.3).
struct ServiceResponse {
    std::uint32_t transactionId = 0;
    std::uint8_t requestSid = 0;
    bool negative = false;
    uwb::protocol::ServiceNrc nrc = uwb::protocol::ServiceNrc::GeneralReject;
    uwb::protocol::ByteBuffer pdu; // positive response PDU including the SID byte

    [[nodiscard]] uwb::protocol::ConstBytes pduBytes() const noexcept { return uwb::protocol::bytesOf(pdu); }
};

using ResponseCallback = std::function<void(ClientResult<ServiceResponse>)>;

// Bookkeeping of one event stream of one connection (specification §31).
struct StreamSubscription {
    std::uint16_t streamId = 0;
    std::uint16_t eventId = 0;
    std::uint8_t mode = static_cast<std::uint8_t>(uwb::protocol::StreamMode::Live);
    std::uint8_t flags = 0;
    std::uint16_t periodMs = 0;
    std::uint16_t queueCapacity = 0;
    uwb::protocol::StreamState state = uwb::protocol::StreamState::Active;
    std::uint32_t droppedCount = 0;
};

// A decoded Event Notification, tagged with the connection that delivered it.
struct EventDelivery {
    domain::DeviceUuid device;
    uwb::protocol::EventNotification event;
};

using EventCallback = std::function<void(const EventDelivery &)>;

} // namespace uwb::client
