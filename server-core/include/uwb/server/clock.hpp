#pragma once

#include <cstdint>

namespace uwb::server {

// Monotonic time source. All server-core timing decisions (session timeout,
// TCP idle timeout, alive-check interval, stream pacing, UART command timeouts)
// are driven through this interface so tests are deterministic
// (implementation plan §18).
//
// Units are microseconds to match the Pico monotonic clock used for event
// timestamps (specification §32, §40.12).
class IClock {
public:
    virtual ~IClock() = default;

    [[nodiscard]] virtual std::uint64_t monotonicUs() const noexcept = 0;
};

// Convenience for handlers that work in milliseconds.
[[nodiscard]] inline constexpr std::uint64_t msToUs(std::uint64_t milliseconds) noexcept {
    return milliseconds * 1000ULL;
}

[[nodiscard]] inline constexpr std::uint64_t usToMs(std::uint64_t microseconds) noexcept {
    return microseconds / 1000ULL;
}

} // namespace uwb::server
