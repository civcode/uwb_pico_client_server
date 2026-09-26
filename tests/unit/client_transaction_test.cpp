#include <catch2/catch_all.hpp>

#include <string>
#include <vector>

#include "uwb/client/transaction.hpp"

using namespace uwb::client;

namespace {

std::vector<RequestId> collect(const std::vector<PendingRequest> &requests) {
    std::vector<RequestId> ids;
    ids.reserve(requests.size());
    for (const PendingRequest &request : requests) {
        ids.push_back(request.transactionId);
    }
    return ids;
}

} // namespace

TEST_CASE("transaction ids are non-zero and increasing per connection", "[unit][client]") {
    TransactionIdAllocator allocator;
    PendingRequestTable table(8);

    auto first = allocator.allocate([&table](RequestId id) { return table.inUse(id); });
    auto second = allocator.allocate([&table](RequestId id) { return table.inUse(id); });
    auto third = allocator.allocate([&table](RequestId id) { return table.inUse(id); });

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(third.has_value());
    CHECK(*first == 1U);
    CHECK(*second == 2U);
    CHECK(*third == 3U);
}

TEST_CASE("transaction id 0 is never allocated", "[unit][client]") {
    TransactionIdAllocator allocator;
    auto used = [](RequestId id) { return id == 1U; };

    auto id = allocator.allocate(used);
    REQUIRE(id.has_value());
    CHECK(*id != 0U);
    CHECK(*id == 2U); // 1 is in use, so it is skipped
}

TEST_CASE("allocated ids skip ids that are still pending", "[unit][client]") {
    TransactionIdAllocator allocator;
    PendingRequestTable table(8);

    for (std::uint32_t value = 1; value <= 4U; ++value) {
        PendingRequest request;
        request.transactionId = value;
        REQUIRE(table.insert(request));
    }

    auto next = allocator.allocate([&table](RequestId id) { return table.inUse(id); });
    REQUIRE(next.has_value());
    CHECK(*next == 5U);

    RequestId taken = table.take(3U).transactionId;
    CHECK(taken == 3U);

    allocator.reset(1);
    auto reused = allocator.allocate([&table](RequestId id) { return table.inUse(id); });
    REQUIRE(reused.has_value());
    CHECK(*reused == 3U); // only a freed id may be reused
}

TEST_CASE("transaction id allocation wraps inside the configured range", "[unit][client]") {
    TransactionIdAllocator allocator(1, 4);
    CHECK(allocator.range() == 4U);

    PendingRequestTable table(8);
    for (std::uint32_t value = 1; value <= 4U; ++value) {
        PendingRequest request;
        request.transactionId = value;
        REQUIRE(table.insert(request));
    }

    // Whole range busy: allocation must refuse rather than reuse a live id.
    auto exhausted = allocator.allocate([&table](RequestId id) { return table.inUse(id); });
    CHECK_FALSE(exhausted.has_value());

    static_cast<void>(table.take(2U));
    allocator.reset(1);
    auto wrapped = allocator.allocate([&table](RequestId id) { return table.inUse(id); });
    REQUIRE(wrapped.has_value());
    CHECK(*wrapped == 2U);
}

TEST_CASE("pending table reports capacity and keeps entries by transaction id", "[unit][client]") {
    PendingRequestTable table(3);
    CHECK(table.capacity() == 3U);

    PendingRequest a;
    a.transactionId = 10;
    PendingRequest b;
    b.transactionId = 11;
    PendingRequest c;
    c.transactionId = 12;

    CHECK(table.insert(a));
    CHECK(table.insert(b));
    CHECK(table.insert(c));
    CHECK(table.full());
    CHECK_FALSE(table.insert(PendingRequest{13, 0x22, 0, 0, true, false, false, {}}));
    CHECK(table.size() == 3U);

    CHECK(table.inUse(11U));
    CHECK_FALSE(table.inUse(99U));
    REQUIRE(table.find(11U) != nullptr);
    CHECK(table.find(11U)->transactionId == 11U);
    CHECK(table.find(99U) == nullptr);
}

TEST_CASE("take returns the entry and clears it", "[unit][client]") {
    PendingRequestTable table(4);
    PendingRequest request;
    request.transactionId = 7;
    request.requestSid = 0x22;
    request.sentAtUs = 100;
    request.deadlineUs = 200;
    bool completed = false;
    request.completion = [&completed](ClientResult<ServiceResponse> result) {
        static_cast<void>(result);
        completed = true;
    };
    REQUIRE(table.insert(request));

    PendingRequest taken = table.take(7U);
    CHECK(taken.transactionId == 7U);
    CHECK(taken.requestSid == 0x22U);
    CHECK(table.size() == 0U);
    CHECK_FALSE(table.inUse(7U));

    // Ownership of the completion moved out with the entry.
    REQUIRE(taken.completion);
    taken.completion(ClientResult<ServiceResponse>::ok(ServiceResponse{}));
    CHECK(completed);

    PendingRequest missing = table.take(99U);
    CHECK(missing.transactionId == kInvalidRequestId);
    CHECK_FALSE(static_cast<bool>(missing.completion));
}

TEST_CASE("expired entries are reclaimed with their completion callback", "[unit][client]") {
    PendingRequestTable table(4);

    PendingRequest early;
    early.transactionId = 1;
    early.sentAtUs = 1000;
    early.deadlineUs = 1500;
    std::vector<RequestId> expired;
    early.completion = [&expired](ClientResult<ServiceResponse> result) {
        expired.push_back(result.ok() ? 0U : 1U);
    };

    PendingRequest later;
    later.transactionId = 2;
    later.sentAtUs = 1000;
    later.deadlineUs = 5000;

    REQUIRE(table.insert(early));
    REQUIRE(table.insert(later));

    std::vector<PendingRequest> reclaimed = table.takeExpired(2000);
    CHECK(collect(reclaimed) == std::vector<RequestId>{1});
    CHECK(table.size() == 1U);
    REQUIRE(reclaimed.size() == 1);
    REQUIRE(reclaimed[0].completion);
    reclaimed[0].completion(ClientResult<ServiceResponse>::error(ErrorDomain::Service, ClientErrorCode::Timeout, "late"));
    CHECK(expired == std::vector<RequestId>{1});

    CHECK(collect(table.takeExpired(5000)) == std::vector<RequestId>{2});
    CHECK(table.size() == 0U);
}

TEST_CASE("late responses are counted instead of delivered", "[unit][client]") {
    PendingRequestTable table(2);
    CHECK(table.lateResponses() == 0U);
    table.recordLateResponse();
    table.recordLateResponse();
    CHECK(table.lateResponses() == 2U);
}

TEST_CASE("takeAll clears the table on connection close", "[unit][client]") {
    PendingRequestTable table(4);
    for (std::uint32_t value = 1; value <= 3U; ++value) {
        PendingRequest request;
        request.transactionId = value;
        REQUIRE(table.insert(request));
    }
    std::vector<PendingRequest> all = table.takeAll();
    CHECK(all.size() == 3U);
    CHECK(table.size() == 0U);
    CHECK_FALSE(table.full());
}
