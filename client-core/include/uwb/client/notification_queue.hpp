#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "uwb/client/client_error.hpp"
#include "uwb/client/client_service.hpp"
#include "uwb/client/client_types.hpp"
#include "uwb/domain/measurement.hpp"

namespace uwb::client {

enum class NotificationKind : std::uint8_t {
    DeviceSeen = 0,
    DeviceRemoved = 1,
    DeviceStateChanged = 2,
    DeviceError = 3,
    Measurement = 4,
    LocalPosition = 5,
    StreamStateChanged = 6,
    CommandFinished = 7,
};

// One queued client notification (specification §59).
struct ClientNotification {
    NotificationKind kind = NotificationKind::DeviceSeen;
    std::optional<DeviceSummary> device;
    std::optional<ClientError> error;
    std::optional<domain::RangeAngleMeasurement> measurement;
    std::optional<domain::LocalPosition> position;
    std::optional<StreamSubscription> stream;
    std::optional<std::string> text;
};

[[nodiscard]] const char *notificationKindName(NotificationKind kind) noexcept;

// Bounded notification queue between the network I/O thread and application
// threads. The I/O thread only ever performs a bounded push, so a slow consumer
// drops notifications instead of stalling the network (plan §29 acceptance).
class NotificationQueue {
public:
    explicit NotificationQueue(std::size_t capacity = 512) noexcept : capacity_(capacity == 0 ? 1 : capacity) {}

    void push(ClientNotification notification) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.size() >= capacity_) {
                queue_.pop_front();
                dropped_++;
            }
            queue_.push_back(std::move(notification));
        }
        cv_.notify_one();
    }

    [[nodiscard]] bool tryPop(ClientNotification &out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return false;
        }
        out = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    [[nodiscard]] bool waitPop(ClientNotification &out, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, timeout, [this] { return !queue_.empty() || closed_; })) {
            return false;
        }
        if (queue_.empty()) {
            return false;
        }
        out = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    [[nodiscard]] std::uint64_t dropped() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    [[nodiscard]] bool closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<ClientNotification> queue_;
    bool closed_ = false;
    std::uint64_t dropped_ = 0;
};

} // namespace uwb::client
