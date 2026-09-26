#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>

#include "uwb/protocol/result.hpp"
#include "uwb/server/clock.hpp"
#include "uwb/server/config_storage.hpp"
#include "uwb/server/logger.hpp"
#include "uwb/server/security_provider.hpp"

namespace uwb::simulator {

using uwb::protocol::ByteBuffer;
using uwb::protocol::ConstBytes;
using uwb::protocol::Result;

// Real monotonic host clock (implementation plan §22.1).
class SystemClock final : public uwb::server::IClock {
public:
    SystemClock() noexcept : start_(std::chrono::steady_clock::now()) {}

    [[nodiscard]] std::uint64_t monotonicUs() const noexcept override {
        const auto elapsed = std::chrono::steady_clock::now() - start_;
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
    }

private:
    std::chrono::steady_clock::time_point start_;
};

// Manually advanced clock used by deterministic simulator runs and tests.
class ManualClock final : public uwb::server::IClock {
public:
    [[nodiscard]] std::uint64_t monotonicUs() const noexcept override { return nowUs_; }

    void advanceUs(std::uint64_t deltaUs) noexcept { nowUs_ += deltaUs; }
    void advanceMs(std::uint64_t deltaMs) noexcept { nowUs_ += deltaMs * 1000ULL; }
    void setUs(std::uint64_t nowUs) noexcept { nowUs_ = nowUs; }

private:
    std::uint64_t nowUs_ = 0;
};

// In-memory IConfigurationStorage stand-in (specification §51 two-phase rule is
// preserved so the same staging path runs on the host as on the Pico).
class MemoryConfigStorage final : public uwb::server::IConfigurationStorage {
public:
    [[nodiscard]] bool available() const noexcept override { return true; }

    [[nodiscard]] Result<ByteBuffer> readActive() override { return Result<ByteBuffer>::ok(active_); }
    [[nodiscard]] Result<ByteBuffer> readBackup() override { return Result<ByteBuffer>::ok(backup_); }

    [[nodiscard]] Result<bool> stage(ConstBytes candidate) override {
        staged_ = ByteBuffer(candidate.begin(), candidate.end());
        hasStaged_ = true;
        return Result<bool>::ok(true);
    }

    [[nodiscard]] Result<bool> commit() override {
        if (!hasStaged_) {
            return Result<bool>::ok(false);
        }
        backup_ = active_;
        active_ = staged_;
        hasStaged_ = false;
        ++commits_;
        return Result<bool>::ok(true);
    }

    void discardStaged() noexcept override {
        staged_.clear();
        hasStaged_ = false;
    }

    [[nodiscard]] Result<bool> restoreBackup() override {
        active_ = backup_;
        return Result<bool>::ok(true);
    }

    void seed(ConstBytes active) {
        active_ = ByteBuffer(active.begin(), active.end());
    }

    [[nodiscard]] std::size_t commitCount() const noexcept { return commits_; }
    [[nodiscard]] const ByteBuffer &active() const noexcept { return active_; }

private:
    ByteBuffer active_;
    ByteBuffer backup_;
    ByteBuffer staged_;
    bool hasStaged_ = false;
    std::size_t commits_ = 0;
};

// Fixed-key Security Access provider (specification §25). The wire format and the
// NRC mapping live in the shared/server layers; this class only supplies the
// algorithm decision.
class StaticSecurityProvider final : public uwb::server::ISecurityProvider {
public:
    explicit StaticSecurityProvider(std::uint32_t expectedKey) noexcept : expectedKey_(expectedKey) {}

    [[nodiscard]] Result<uwb::server::SecuritySeed> createSeed(uwb::server::ConnectionId connection) override {
        ++seedCalls_;
        seedCounter_ = seedCounter_ == 0 ? 0x1000U : seedCounter_ + 1U;
        seeds_[connection] = seedCounter_;
        return Result<uwb::server::SecuritySeed>::ok(seedCounter_);
    }

    [[nodiscard]] uwb::server::SecurityOutcome verifyKey(uwb::server::ConnectionId connection,
                                                         uwb::server::SecuritySeed seed,
                                                         uwb::server::SecurityKey key) override {
        ++verifyCalls_;
        const auto it = seeds_.find(connection);
        if (it == seeds_.end() || it->second != seed) {
            return uwb::server::SecurityOutcome{uwb::server::SecurityDecision::Denied, 0};
        }
        seeds_.erase(it); // single use (docs/protocol_decisions.md §16)
        if (key != expectedKey_) {
            return uwb::server::SecurityOutcome{uwb::server::SecurityDecision::BadKey, 0};
        }
        return uwb::server::SecurityOutcome{uwb::server::SecurityDecision::Accepted, 0};
    }

    void reset(uwb::server::ConnectionId connection) noexcept override { seeds_.erase(connection); }

    void noteFailure(uwb::server::ConnectionId) noexcept override { ++failures_; }

    [[nodiscard]] std::size_t seedCallCount() const noexcept { return seedCalls_; }
    [[nodiscard]] std::size_t verifyCallCount() const noexcept { return verifyCalls_; }
    [[nodiscard]] std::size_t failureCount() const noexcept { return failures_; }

private:
    std::uint32_t expectedKey_ = 0;
    std::uint32_t seedCounter_ = 0;
    std::uint32_t seedCalls_ = 0;
    std::uint32_t verifyCalls_ = 0;
    std::uint32_t failures_ = 0;
    std::unordered_map<uwb::server::ConnectionId, uwb::server::SecuritySeed> seeds_;
};

// Console logger.
class ConsoleLogger final : public uwb::server::ILogger {
public:
    explicit ConsoleLogger(bool enabled) noexcept : enabled_(enabled) {}

    void log(uwb::server::LogLevel level, std::string_view message) noexcept override {
        if (!enabled_) {
            return;
        }
        const char *prefix = "info";
        switch (level) {
        case uwb::server::LogLevel::Debug:
            prefix = "dbg";
            break;
        case uwb::server::LogLevel::Info:
            prefix = "info";
            break;
        case uwb::server::LogLevel::Warn:
            prefix = "warn";
            break;
        case uwb::server::LogLevel::Error:
            prefix = "err ";
            break;
        }
        std::printf("[sim %s] %.*s\n", prefix, static_cast<int>(message.size()), message.data());
        std::fflush(stdout);
    }

private:
    bool enabled_ = false;
};

} // namespace uwb::simulator
