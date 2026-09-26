#pragma once

#include <cstdint>

#include "uwb/protocol/constants.hpp"
#include "uwb/protocol/did_records.hpp"
#include "uwb/protocol/result.hpp"
#include "uwb/protocol/service_ids.hpp"

namespace uwb::server {

// Connection slot limits (specification §9.1). Configurable, but the v1 default
// is one Control connection and three Observers.
struct ConnectionLimits {
    std::uint32_t maxControlConnections = 1;
    std::uint32_t maxObserverConnections = 3;
    std::uint32_t maxTcpConnections = 4; // must be >= control + observer slots
};

// Timeout configuration (specification §52). Values are validated against the
// documented bounds; out-of-bounds values are rejected rather than clamped.
struct TimeoutSettings {
    std::uint32_t uartCommandTimeoutMs = uwb::protocol::kDefaultUartCommandTimeoutMs;
    std::uint32_t uartInterCommandGapMs = uwb::protocol::kDefaultUartInterCommandGapMs;
    std::uint32_t sessionInactivityTimeoutMs = uwb::protocol::kDefaultSessionInactivityMs;
    std::uint32_t tcpIdleTimeoutMs = uwb::protocol::kDefaultTcpIdleTimeoutMs;
    std::uint32_t aliveCheckIntervalMs = uwb::protocol::kDefaultAliveCheckIntervalMs;
};

// Bounded queue capacities (specification §53).
struct QueueCapacities {
    std::uint32_t uwbCommandQueue = uwb::protocol::kUwbCommandQueueCapacity;
    std::uint32_t connectionHighPriorityTx = uwb::protocol::kConnectionHighPriorityTxCapacity;
    std::uint32_t connectionNormalPriorityTx = uwb::protocol::kConnectionNormalPriorityTxCapacity;
    std::uint32_t liveStreamQueue = uwb::protocol::kLiveStreamQueueCapacity;
    std::uint32_t recordingStreamQueue = uwb::protocol::kRecordingStreamQueueMinCapacity;
    std::uint32_t uartRxRingBuffer = uwb::protocol::kUartRxRingBufferSize;
};

// Server-wide configuration.
struct ServerConfig {
    ConnectionLimits limits{};
    TimeoutSettings timeouts{};
    QueueCapacities queues{};

    // Security policy. When false, CAP_SECURITY_ACCESS stays clear and service
    // 0x27 answers with a service-not-supported NRC (specification §25).
    bool securityEnabled = false;

    // Raw AT policy. Raw AT additionally needs CAP_RAW_AT and Extended Session
    // (specification §30).
    bool rawAtEnabled = false;

    // Maximum events per second per connection (server-side clamp, §31.3).
    std::uint16_t maxEventsPerSecond = 100;

    // Maximum concurrent streams per connection (§31.4 QueueCapacity reporting).
    std::uint16_t maxStreamsPerConnection = 4;

    // Service response timeouts reported by Session Control (§23).
    std::uint16_t p2ServerMaxMs = uwb::protocol::kDefaultP2ServerMaxMs;
    std::uint16_t p2StarServerMax10ms = uwb::protocol::kDefaultP2StarServerMax10ms;
};

// Rejects out-of-bounds timeouts and inconsistent limits (specification §52).
[[nodiscard]] uwb::protocol::Result<bool> validateTimeouts(const TimeoutSettings &timeouts) noexcept;
[[nodiscard]] uwb::protocol::Result<bool> validateConnectionLimits(const ConnectionLimits &limits) noexcept;
[[nodiscard]] uwb::protocol::Result<bool> validateQueueCapacities(const QueueCapacities &queues) noexcept;
[[nodiscard]] uwb::protocol::Result<bool> validateServerConfig(const ServerConfig &config) noexcept;

// Mapping to/from the 0xF009 Timeout Configuration DID record (§40.10).
[[nodiscard]] uwb::protocol::TimeoutConfigRecord toTimeoutRecord(const TimeoutSettings &timeouts) noexcept;
[[nodiscard]] TimeoutSettings fromTimeoutRecord(const uwb::protocol::TimeoutConfigRecord &record) noexcept;

} // namespace uwb::server
