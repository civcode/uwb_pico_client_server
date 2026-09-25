#include <cstdint>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "uwb/domain/version.hpp"
#include "uwb/protocol/version.hpp"
#include "uwb/server/version.hpp"

TEST_CASE("Phase 0 build wiring links the shared layers", "[phase0][build]") {
    CHECK(uwb::protocol::kWireProtocolVersion == 1);
    CHECK_FALSE(uwb::protocol::libraryVersion().empty());
    CHECK_FALSE(uwb::domain::domainLibraryVersion().empty());
    CHECK_FALSE(uwb::server::serverCoreVersion().empty());
}

TEST_CASE("host smoke test can pass a byte span around", "[phase0][build]") {
    std::vector<std::uint8_t> bytes{0xde, 0xad, 0xbe, 0xef};
    std::span<const std::uint8_t> view = bytes;
    CHECK(view.size() == 4);
    CHECK(view[0] == 0xde);
}
