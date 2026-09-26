#pragma once

#include <cstdint>

#include "uwb/protocol/result.hpp"
#include "uwb/server/transport.hpp"

namespace uwb::server {

// Security Access provider (specification §25). The wire format fixes only the
// sub-functions and the 4-byte seed/key width (docs/protocol_decisions.md §6);
// the algorithm itself lives behind this interface so it can be replaced without
// touching the codecs.
using SecuritySeed = std::uint32_t;
using SecurityKey = std::uint32_t;

inline constexpr std::size_t kSecuritySeedSize = 4;
inline constexpr std::size_t kSecurityKeySize = 4;

// Outcome of a key verification attempt. The dispatcher maps these onto the
// §21.4 NRCs; a provider never encodes a response itself.
enum class SecurityDecision : std::uint8_t {
    Accepted = 0,
    BadKey = 1,               // 0x35 invalidKey
    Denied = 2,               // 0x33 securityAccessDenied
    TooManyAttempts = 3,      // 0x36 exceedNumberOfAttempts
    DelayNotExpired = 4,      // 0x37 requiredTimeDelayNotExpired
    Failed = 5,               // 0x10 generalReject
};

struct SecurityOutcome {
    SecurityDecision decision = SecurityDecision::Accepted;
    std::uint32_t retryAfterMs = 0; // 0 when not throttled
};

class ISecurityProvider {
public:
    virtual ~ISecurityProvider() = default;

    // Called on "request seed". The provider associates the seed with the
    // connection (and MAY expire it using IClock).
    [[nodiscard]] virtual uwb::protocol::Result<SecuritySeed> createSeed(ConnectionId connection) = 0;

    // Called on "send key".
    [[nodiscard]] virtual SecurityOutcome verifyKey(ConnectionId connection, SecuritySeed seed, SecurityKey key) = 0;

    // Called when a connection drops or its session returns to Default.
    virtual void reset(ConnectionId connection) noexcept = 0;

    // Called after a failed attempt so the provider can record attempts/lockout.
    virtual void noteFailure(ConnectionId connection) noexcept = 0;
};

} // namespace uwb::server
