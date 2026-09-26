#pragma once

#include <cstdint>

#include "uwb/protocol/bytes.hpp"

namespace uwb::server {

// Identity of one TCP connection as seen by the server core. The core never
// touches sockets: the host application (simulator, CLI test harness) or the Pico
// lwIP glue layer creates a connection, feeds bytes in, and receives complete
// encoded frames through IConnectionWriter (implementation plan §16.1, §18).
using ConnectionId = std::uint32_t;

inline constexpr ConnectionId kInvalidConnectionId = 0;

// Minimal endpoint metadata. The authoritative IP endpoint lives in the
// discovery layer (specification §17: "the UDP source address is the
// authoritative current IP endpoint"), so the core does not carry addresses.
struct PeerInfo {
    std::uint16_t remotePort = 0;
};

// Outbound byte sink for one connection. Implementations MUST be non-blocking:
// the server core schedules frames and relies on the transport reporting how much
// it accepted, so a stalled peer can never stall the protocol loop
// (specification §38, implementation plan §18).
class IConnectionWriter {
public:
    virtual ~IConnectionWriter() = default;

    // Attempt to hand one complete frame (generic header + payload) to the
    // transport. Returns true when the frame was accepted for transmission.
    virtual bool writeFrame(uwb::protocol::ConstBytes frame) = 0;

    [[nodiscard]] virtual bool isOpen() const noexcept = 0;
};

} // namespace uwb::server
