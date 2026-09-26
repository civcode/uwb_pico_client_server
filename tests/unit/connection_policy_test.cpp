#include <catch2/catch_all.hpp>

#include "uwb/server/connection_context.hpp"

using namespace uwb::server;
using uwb::protocol::ActivationResponseCode;
using uwb::protocol::ConnectionRole;

namespace {

ConnectionRegistry registryWithLimits(std::uint32_t control, std::uint32_t observers, std::uint32_t total) {
    ConnectionLimits limits;
    limits.maxControlConnections = control;
    limits.maxObserverConnections = observers;
    limits.maxTcpConnections = total;
    return ConnectionRegistry(limits);
}

} // namespace

TEST_CASE("first Control activation gets the Control slot", "[unit][server][connections]") {
    auto reg = registryWithLimits(1, 3, 4);
    reg.open(1, PeerInfo{}, 100);
    reg.open(2, PeerInfo{}, 100);

    const auto first = reg.assignRole(1, ConnectionRole::Control, 110);
    CHECK(first.accepted);
    CHECK(first.assigned == ConnectionRole::Control);
    CHECK(first.code == ActivationResponseCode::AcceptedRequestedRole);
    CHECK(reg.controlCount() == 1);
    CHECK(reg.find(1)->isControl());
}

TEST_CASE("Control request while the Control slot is taken is downgraded to Observer",
          "[unit][server][connections]") {
    auto reg = registryWithLimits(1, 3, 4);
    reg.open(1, PeerInfo{}, 100);
    reg.open(2, PeerInfo{}, 100);

    (void)reg.assignRole(1, ConnectionRole::Control, 110);
    const auto second = reg.assignRole(2, ConnectionRole::Control, 120);

    // Specification §9.3 + §18.2 code 0x01: accepted, but as Observer.
    CHECK(second.accepted);
    CHECK(second.assigned == ConnectionRole::Observer);
    CHECK(second.code == ActivationResponseCode::AcceptedDowngradedToObserver);
    CHECK(reg.find(2)->isObserver());
    CHECK(reg.controlCount() == 1);
}

TEST_CASE("Control request is rejected when no Observer slot is left either",
          "[unit][server][connections]") {
    auto reg = registryWithLimits(1, 1, 2);
    reg.open(1, PeerInfo{}, 100);
    reg.open(2, PeerInfo{}, 100);

    (void)reg.assignRole(1, ConnectionRole::Control, 110);
    (void)reg.assignRole(2, ConnectionRole::Observer, 120);

    reg.open(3, PeerInfo{}, 130);
    const auto third = reg.assignRole(3, ConnectionRole::Control, 140);
    CHECK_FALSE(third.accepted);
    CHECK(third.code == ActivationResponseCode::RejectedNoConnectionSlot);
    CHECK(third.assigned == ConnectionRole::None);
}

TEST_CASE("Observer slots are capped", "[unit][server][connections]") {
    auto reg = registryWithLimits(1, 2, 4);
    reg.open(1, PeerInfo{}, 100);
    reg.open(2, PeerInfo{}, 100);
    reg.open(3, PeerInfo{}, 100);

    const auto a = reg.assignRole(1, ConnectionRole::Observer, 110);
    const auto b = reg.assignRole(2, ConnectionRole::Observer, 110);
    const auto c = reg.assignRole(3, ConnectionRole::Observer, 110);

    CHECK(a.accepted);
    CHECK(b.accepted);
    CHECK_FALSE(c.accepted);
    CHECK(c.code == ActivationResponseCode::RejectedNoConnectionSlot);
    CHECK(reg.observerCount() == 2);
}

TEST_CASE("total TCP connection cap is enforced", "[unit][server][connections]") {
    auto reg = registryWithLimits(1, 3, 2);

    reg.open(1, PeerInfo{}, 100);
    CHECK(reg.assignRole(1, ConnectionRole::Observer, 110).accepted);
    reg.open(2, PeerInfo{}, 120);
    CHECK(reg.assignRole(2, ConnectionRole::Observer, 130).accepted);

    // A third socket exceeds maxTcpConnections: no slot, activation refused.
    reg.open(3, PeerInfo{}, 140);
    const auto third = reg.assignRole(3, ConnectionRole::Observer, 150);
    CHECK_FALSE(third.accepted);
    CHECK(third.code == ActivationResponseCode::RejectedNoConnectionSlot);
}

TEST_CASE("observers are never promoted implicitly", "[unit][server][connections]") {
    auto reg = registryWithLimits(1, 3, 4);
    reg.open(1, PeerInfo{}, 100);
    (void)reg.assignRole(1, ConnectionRole::Observer, 110);
    CHECK(reg.find(1)->isObserver());

    // Re-activation as Control while the Control slot is free is allowed (§9.3).
    const auto promoted = reg.assignRole(1, ConnectionRole::Control, 120);
    CHECK(promoted.accepted);
    CHECK(promoted.assigned == ConnectionRole::Control);

    // A second Control request now conflicts with the active Control role.
    reg.open(2, PeerInfo{}, 130);
    const auto clash = reg.assignRole(2, ConnectionRole::Control, 140);
    CHECK(clash.accepted);
    CHECK(clash.assigned == ConnectionRole::Observer);
}

TEST_CASE("releasing Control frees the slot for another connection", "[unit][server][connections]") {
    auto reg = registryWithLimits(1, 3, 4);
    reg.open(1, PeerInfo{}, 100);
    reg.open(2, PeerInfo{}, 100);
    (void)reg.assignRole(1, ConnectionRole::Control, 110);

    CHECK(reg.releaseControl(1));
    CHECK(reg.controlCount() == 0);
    CHECK(reg.find(1)->state == ConnectionState::Closing); // §9.4 flush-then-close

    const auto next = reg.assignRole(2, ConnectionRole::Control, 120);
    CHECK(next.accepted);
    CHECK(next.assigned == ConnectionRole::Control);
}

TEST_CASE("closing a connection releases its slot", "[unit][server][connections]") {
    auto reg = registryWithLimits(1, 1, 2);
    reg.open(1, PeerInfo{}, 100);
    reg.open(2, PeerInfo{}, 100);
    (void)reg.assignRole(1, ConnectionRole::Control, 110);
    (void)reg.assignRole(2, ConnectionRole::Observer, 110);

    CHECK_FALSE(reg.hasObserverSlot());
    reg.close(2);
    CHECK(reg.observerCount() == 0);
    CHECK(reg.hasObserverSlot());
}

TEST_CASE("session and security state resets with Default Session", "[unit][server][connections]") {
    auto reg = registryWithLimits(1, 3, 4);
    reg.open(1, PeerInfo{}, 100);
    (void)reg.assignRole(1, ConnectionRole::Control, 110);

    reg.enterExtendedSession(1, 200);
    auto *ctx = reg.find(1);
    REQUIRE(ctx != nullptr);
    ctx->security = SecurityState::Unlocked;
    ctx->seed = 0xDEADBEEFU;

    reg.enterDefaultSession(1);
    CHECK(ctx->session == SessionState::Default);
    CHECK(ctx->security == SecurityState::Locked); // §22.2: Default Session re-locks
    CHECK(ctx->seed == 0);
}

TEST_CASE("validateConnectionLimits rejects inconsistent slot counts", "[unit][server][connections]") {
    ConnectionLimits bad;
    bad.maxControlConnections = 2;
    bad.maxObserverConnections = 3;
    bad.maxTcpConnections = 4; // < control + observers

    const auto result = validateConnectionLimits(bad);
    CHECK_FALSE(result.ok());
}
