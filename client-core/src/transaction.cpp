#include "uwb/client/transaction.hpp"

namespace uwb::client {

// ---------------------------------------------------------------------------
// TransactionIdAllocator (plan §25.1)
// ---------------------------------------------------------------------------

std::optional<RequestId> TransactionIdAllocator::allocate(const std::function<bool(RequestId)> &inUse) {
    const std::uint64_t span = static_cast<std::uint64_t>(maximum_) - static_cast<std::uint64_t>(first_) + 1ULL;

    // A full span means every id is still outstanding: the caller must reject
    // the request rather than reuse an id (§20.1 correlation rule).
    for (std::uint64_t probe = 0; probe < span; ++probe) {
        const RequestId candidate = next_;
        next_ = next_ == maximum_ ? first_ : static_cast<RequestId>(next_ + 1U);
        if (candidate == kInvalidRequestId) {
            continue;
        }
        if (inUse(candidate)) {
            continue;
        }
        return candidate;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// PendingRequestTable (plan §25.1, specification §56)
// ---------------------------------------------------------------------------

bool PendingRequestTable::insert(PendingRequest request) {
    if (entries_.size() >= capacity_) {
        return false;
    }
    entries_.push_back(std::move(request));
    return true;
}

PendingRequest *PendingRequestTable::find(RequestId transactionId) noexcept {
    for (PendingRequest &entry : entries_) {
        if (entry.transactionId == transactionId) {
            return &entry;
        }
    }
    return nullptr;
}

bool PendingRequestTable::inUse(RequestId transactionId) const noexcept {
    for (const PendingRequest &entry : entries_) {
        if (entry.transactionId == transactionId) {
            return true;
        }
    }
    return false;
}

PendingRequest PendingRequestTable::take(RequestId transactionId) {
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (entries_[index].transactionId == transactionId) {
            PendingRequest entry = std::move(entries_[index]);
            entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));
            return entry;
        }
    }
    return PendingRequest{};
}

std::vector<PendingRequest> PendingRequestTable::takeExpired(std::uint64_t nowUs) {
    std::vector<PendingRequest> expired;
    for (std::size_t index = 0; index < entries_.size();) {
        if (entries_[index].deadlineUs <= nowUs) {
            expired.push_back(std::move(entries_[index]));
            entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));
            continue;
        }
        ++index;
    }
    return expired;
}

std::vector<PendingRequest> PendingRequestTable::takeAll() {
    std::vector<PendingRequest> all = std::move(entries_);
    entries_.clear();
    return all;
}

} // namespace uwb::client
