#include "uwb/server/server_config.hpp"

#include "uwb/protocol/constants.hpp"
#include "uwb/protocol/errors.hpp"

namespace uwb::server {

using uwb::protocol::ProtocolError;
using uwb::protocol::ProtocolErrorCode;
using uwb::protocol::Result;

namespace {

Result<bool> outOfBounds(std::uint32_t value) noexcept {
    return Result<bool>::error(ProtocolError{ProtocolErrorCode::InvalidField, value});
}

bool inRange(std::uint32_t value, std::uint32_t min, std::uint32_t max) noexcept {
    return value >= min && value <= max;
}

} // namespace

Result<bool> validateTimeouts(const TimeoutSettings &timeouts) noexcept {
    if (!inRange(timeouts.uartCommandTimeoutMs, uwb::protocol::kMinUartCommandTimeoutMs,
                 uwb::protocol::kMaxUartCommandTimeoutMs)) {
        return outOfBounds(timeouts.uartCommandTimeoutMs);
    }
    if (!inRange(timeouts.uartInterCommandGapMs, uwb::protocol::kMinUartInterCommandGapMs,
                 uwb::protocol::kMaxUartInterCommandGapMs)) {
        return outOfBounds(timeouts.uartInterCommandGapMs);
    }
    if (!inRange(timeouts.sessionInactivityTimeoutMs, uwb::protocol::kMinSessionInactivityMs,
                 uwb::protocol::kMaxSessionInactivityMs)) {
        return outOfBounds(timeouts.sessionInactivityTimeoutMs);
    }
    if (!inRange(timeouts.tcpIdleTimeoutMs, uwb::protocol::kMinTcpIdleTimeoutMs,
                 uwb::protocol::kMaxTcpIdleTimeoutMs)) {
        return outOfBounds(timeouts.tcpIdleTimeoutMs);
    }
    if (!inRange(timeouts.aliveCheckIntervalMs, uwb::protocol::kMinAliveCheckIntervalMs,
                 uwb::protocol::kMaxAliveCheckIntervalMs)) {
        return outOfBounds(timeouts.aliveCheckIntervalMs);
    }
    return Result<bool>::ok(true);
}

Result<bool> validateConnectionLimits(const ConnectionLimits &limits) noexcept {
    if (limits.maxControlConnections == 0) {
        return outOfBounds(0);
    }
    // v1 supports exactly one Control connection (specification §9.1).
    if (limits.maxControlConnections != 1) {
        return outOfBounds(limits.maxControlConnections);
    }
    if (limits.maxObserverConnections > uwb::protocol::kMaxObserverConnections) {
        return outOfBounds(limits.maxObserverConnections);
    }
    if (limits.maxTcpConnections < limits.maxControlConnections + limits.maxObserverConnections) {
        return outOfBounds(limits.maxTcpConnections);
    }
    return Result<bool>::ok(true);
}

Result<bool> validateQueueCapacities(const QueueCapacities &queues) noexcept {
    if (queues.uwbCommandQueue == 0) {
        return outOfBounds(0);
    }
    if (queues.connectionHighPriorityTx == 0 || queues.connectionNormalPriorityTx == 0) {
        return outOfBounds(0);
    }
    if (queues.liveStreamQueue == 0) {
        return outOfBounds(queues.liveStreamQueue);
    }
    if (queues.recordingStreamQueue < uwb::protocol::kRecordingStreamQueueMinCapacity) {
        return outOfBounds(queues.recordingStreamQueue);
    }
    if (queues.uartRxRingBuffer == 0) {
        return outOfBounds(queues.uartRxRingBuffer);
    }
    return Result<bool>::ok(true);
}

Result<bool> validateServerConfig(const ServerConfig &config) noexcept {
    if (auto r = validateConnectionLimits(config.limits); !r.ok()) {
        return r;
    }
    if (auto r = validateTimeouts(config.timeouts); !r.ok()) {
        return r;
    }
    if (auto r = validateQueueCapacities(config.queues); !r.ok()) {
        return r;
    }
    if (config.maxStreamsPerConnection == 0 || config.maxEventsPerSecond == 0) {
        return outOfBounds(0);
    }
    return Result<bool>::ok(true);
}

uwb::protocol::TimeoutConfigRecord toTimeoutRecord(const TimeoutSettings &timeouts) noexcept {
    uwb::protocol::TimeoutConfigRecord record;
    record.uartCommandTimeoutMs = timeouts.uartCommandTimeoutMs;
    record.uartInterCommandGapMs = timeouts.uartInterCommandGapMs;
    record.sessionInactivityTimeoutMs = timeouts.sessionInactivityTimeoutMs;
    record.tcpIdleTimeoutMs = timeouts.tcpIdleTimeoutMs;
    record.aliveCheckIntervalMs = timeouts.aliveCheckIntervalMs;
    return record;
}

TimeoutSettings fromTimeoutRecord(const uwb::protocol::TimeoutConfigRecord &record) noexcept {
    TimeoutSettings timeouts;
    timeouts.uartCommandTimeoutMs = record.uartCommandTimeoutMs;
    timeouts.uartInterCommandGapMs = record.uartInterCommandGapMs;
    timeouts.sessionInactivityTimeoutMs = record.sessionInactivityTimeoutMs;
    timeouts.tcpIdleTimeoutMs = record.tcpIdleTimeoutMs;
    timeouts.aliveCheckIntervalMs = record.aliveCheckIntervalMs;
    return timeouts;
}

} // namespace uwb::server
