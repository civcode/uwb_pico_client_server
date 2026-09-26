#pragma once

#include <cstdint>

namespace uwb::client {

// Monotonic clock interface for the client layer. All client timeouts and
// deadlines are monotonic microseconds, never wall-clock time.
class IClock {
public:
    virtual ~IClock() = default;
    [[nodiscard]] virtual std::uint64_t nowUs() const noexcept = 0;
};

class SystemClock final : public IClock {
public:
    [[nodiscard]] std::uint64_t nowUs() const noexcept override;
};

// Manually advanced clock used by deterministic unit tests.
class ManualClock final : public IClock {
public:
    explicit ManualClock(std::uint64_t startUs = 1000000) noexcept : nowUs_(startUs) {}

    [[nodiscard]] std::uint64_t nowUs() const noexcept override { return nowUs_; }

    void advanceUs(std::uint64_t deltaUs) noexcept { nowUs_ += deltaUs; }
    void advanceMs(std::uint64_t deltaMs) noexcept { nowUs_ += deltaMs * 1000ULL; }
    void setUs(std::uint64_t value) noexcept { nowUs_ = value; }

private:
    std::uint64_t nowUs_;
};

} // namespace uwb::client
