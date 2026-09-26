#include "uwb/client/client_clock.hpp"

#include <chrono>

namespace uwb::client {

std::uint64_t SystemClock::nowUs() const noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

} // namespace uwb::client
