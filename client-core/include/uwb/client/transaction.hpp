#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "uwb/client/client_service.hpp"

namespace uwb::client {

using RequestId = std::uint32_t;

inline constexpr RequestId kInvalidRequestId = 0; // transaction id 0 is reserved (§20.1)

// Transaction ID allocation per TCP connection (specification §20.1):
// non-zero, wrapping, and never reusing an ID that is still pending.
// The maximum is injectable so that wrap behaviour is testable with a small
// range (implementation plan §28).
class TransactionIdAllocator {
public:
    explicit TransactionIdAllocator(std::uint32_t first = 1,
                                    std::uint32_t maximum = 0xFFFFFFFFU) noexcept
        : first_(first == 0 ? 1 : first), maximum_(maximum < first ? first : maximum), next_(first) {}

    // Returns an unused id, or std::nullopt when the whole usable range is
    // occupied (i.e. too many outstanding requests for this id space).
    [[nodiscard]] std::optional<RequestId> allocate(const std::function<bool(RequestId)> &inUse);

    [[nodiscard]] std::uint32_t nextValue() const noexcept { return next_; }
    [[nodiscard]] std::uint32_t range() const noexcept { return maximum_ - first_ + 1U; }

    void reset(std::uint32_t next = 1) noexcept { next_ = next == 0 ? first_ : next; }

private:
    std::uint32_t first_;
    std::uint32_t maximum_;
    std::uint32_t next_;
};

// Pending request table (specification §56): correlation happens by
// transaction ID, never by arrival order.
struct PendingRequest {
    RequestId transactionId = kInvalidRequestId;
    std::uint8_t requestSid = 0;
    std::uint64_t sentAtUs = 0;
    std::uint64_t deadlineUs = 0;
    bool sent = false;
    bool cancelled = false;
    bool sawResponsePending = false;
    ResponseCallback completion;
};

class PendingRequestTable {
public:
    explicit PendingRequestTable(std::uint32_t capacity = 16) noexcept : capacity_(capacity == 0 ? 1 : capacity) {}

    [[nodiscard]] bool insert(PendingRequest request);
    [[nodiscard]] PendingRequest *find(RequestId transactionId) noexcept;
    [[nodiscard]] bool inUse(RequestId transactionId) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool full() const noexcept { return entries_.size() >= capacity_; }

    // Removes and returns the entry, or an empty entry when it is unknown.
    [[nodiscard]] PendingRequest take(RequestId transactionId);

    // Removes every entry whose deadline has passed.
    [[nodiscard]] std::vector<PendingRequest> takeExpired(std::uint64_t nowUs);

    // Removes everything (connection close).
    [[nodiscard]] std::vector<PendingRequest> takeAll();

    // Late responses are counted, not delivered (§74 category 2).
    void recordLateResponse() noexcept { ++lateResponses_; }
    [[nodiscard]] std::uint64_t lateResponses() const noexcept { return lateResponses_; }

    [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }
    void setCapacity(std::uint32_t capacity) noexcept { capacity_ = capacity == 0 ? 1 : capacity; }

private:
    std::uint32_t capacity_;
    std::uint64_t lateResponses_ = 0;
    // Linear by design: the outstanding-request count per connection is small
    // (bounded by capacity), so a plain vector beats a hash map here.
    std::vector<PendingRequest> entries_;
};

} // namespace uwb::client
