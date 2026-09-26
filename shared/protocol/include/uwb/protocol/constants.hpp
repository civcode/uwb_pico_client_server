#pragma once

#include <cstddef>
#include <cstdint>

namespace uwb::protocol {

// Specification §11.3
inline constexpr std::size_t kGenericHeaderSize = 8;
inline constexpr std::uint32_t kMaxProtocolPayload = 4096;
inline constexpr std::size_t kMaxFrameSize = kGenericHeaderSize + kMaxProtocolPayload;

// Specification §7 — compile-time configurable.
#ifndef UWB_DEFAULT_PORT
#define UWB_DEFAULT_PORT 13401
#endif
inline constexpr std::uint16_t kDefaultProtocolPort = static_cast<std::uint16_t>(UWB_DEFAULT_PORT);

// Specification §30
inline constexpr std::uint16_t kMaxAtCommandLength = 512;

// Specification §40.3
inline constexpr std::size_t kMaxDeviceNameLength = 32;

// Specification §20.1 / §21.2
inline constexpr std::uint8_t kPositiveResponseSidOffset = 0x40;
inline constexpr std::uint8_t kNegativeResponseSid = 0x7F;


// Server timing defaults and bounds (specification §52).
inline constexpr std::uint32_t kDefaultUartCommandTimeoutMs = 1000;
inline constexpr std::uint32_t kMinUartCommandTimeoutMs = 100;
inline constexpr std::uint32_t kMaxUartCommandTimeoutMs = 10000;

inline constexpr std::uint32_t kDefaultUartInterCommandGapMs = 100;
inline constexpr std::uint32_t kMinUartInterCommandGapMs = 0;
inline constexpr std::uint32_t kMaxUartInterCommandGapMs = 1000;

inline constexpr std::uint32_t kDefaultSessionInactivityMs = 5000;
inline constexpr std::uint32_t kMinSessionInactivityMs = 1000;
inline constexpr std::uint32_t kMaxSessionInactivityMs = 600000;

inline constexpr std::uint32_t kDefaultTcpIdleTimeoutMs = 60000;
inline constexpr std::uint32_t kMinTcpIdleTimeoutMs = 5000;
inline constexpr std::uint32_t kMaxTcpIdleTimeoutMs = 3600000;

inline constexpr std::uint32_t kDefaultAliveCheckIntervalMs = 10000;
inline constexpr std::uint32_t kMinAliveCheckIntervalMs = 1000;
inline constexpr std::uint32_t kMaxAliveCheckIntervalMs = 600000;

// Bounded queue capacities (specification §53).
inline constexpr std::uint32_t kUwbCommandQueueCapacity = 8;
inline constexpr std::uint32_t kConnectionHighPriorityTxCapacity = 8;
inline constexpr std::uint32_t kConnectionNormalPriorityTxCapacity = 8;
inline constexpr std::uint32_t kLiveStreamQueueCapacity = 4;
inline constexpr std::uint32_t kRecordingStreamQueueMinCapacity = 32;
inline constexpr std::uint32_t kUartRxRingBufferSize = 2048;

} // namespace uwb::protocol
