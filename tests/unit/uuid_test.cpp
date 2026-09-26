#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "uwb/protocol/uuid.hpp"

#include "gen/golden_vectors.hpp"
#include "test_helpers.hpp"

using namespace uwb::protocol;
using uwb::test::sameBytes;
using uwb::test::view;

TEST_CASE("Device UUID derivation golden vector (§6.2)", "[protocol][uuid][golden]") {
    auto derived = deviceUuidFromBoardId(view(uwb::test::kBoardId));
    REQUIRE(derived.ok());
    CHECK(derived->bytes == uwb::test::kDeviceUuid);

    // The namespace itself must match the documented project namespace.
    CHECK(deviceUuidNamespace().bytes == uwb::test::kNamespaceUuid);
}

TEST_CASE("Device UUID derivation works from a u64 board id", "[protocol][uuid]") {
    // e661640f7c123456 with id[0] as the most significant byte.
    auto derived = deviceUuidFromBoardIdU64(0xE661640F7C123456ULL);
    CHECK(derived.bytes == uwb::test::kDeviceUuid);
}

TEST_CASE("Device UUID name is the lowercase hex of the board id (§6.2)", "[protocol][uuid]") {
    const std::array<std::uint8_t, 8> boardId = {0xE6, 0x61, 0x64, 0x0F, 0x7C, 0x12, 0x34, 0x56};
    auto viaBytes = deviceUuidFromBoardId(view(boardId));
    REQUIRE(viaBytes.ok());

    // The same UUID must result from explicitly running UUIDv5 over the hex name.
    const std::string name = "e661640f7c123456";
    auto viaName = uuidV5(deviceUuidNamespace(),
                          ConstBytes{reinterpret_cast<const std::uint8_t *>(name.data()), name.size()});
    CHECK(viaName.bytes == viaBytes->bytes);
}

TEST_CASE("UUIDv5 matches the RFC 9562 Appendix A.4 vector", "[protocol][uuid]") {
    // RFC 9562 A.4: DNS namespace + "www.example.com"
    // SHA-1 = 2ed6657de927468b55e12665a8aea6a22dee3e35
    // final = 2ed6657d-e927-568b-95e1-2665a8aea6a2
    auto ns = uuidFromString("6ba7b810-9dad-11d1-80b4-00c04fd430c8");
    REQUIRE(ns.ok());
    const std::string name = "www.example.com";
    auto result = uuidV5(*ns, ConstBytes{reinterpret_cast<const std::uint8_t *>(name.data()), name.size()});

    auto expected = uuidFromString("2ed6657d-e927-568b-95e1-2665a8aea6a2");
    REQUIRE(expected.ok());
    CHECK(result.bytes == expected->bytes);

    // Version and variant nibbles.
    CHECK((result.bytes[6] & 0xF0U) == 0x50U);
    CHECK((result.bytes[8] & 0xC0U) == 0x80U);
}

TEST_CASE("UUID formatting and parsing round-trip", "[protocol][uuid]") {
    Uuid uuid{};
    for (std::size_t i = 0; i < uuid.bytes.size(); ++i) {
        uuid.bytes[i] = static_cast<std::uint8_t>(i * 0x11U);
    }

    char text[37] = {};
    uuid.formatLowerHex(text, sizeof(text));
    CHECK(std::string_view{text} == "00112233-4455-6677-8899-aabbccddeeff");

    auto parsed = uuidFromString(text);
    REQUIRE(parsed.ok());
    CHECK(parsed->bytes == uuid.bytes);
}

TEST_CASE("UUID parsing rejects malformed input", "[protocol][uuid]") {
    CHECK(uuidFromString("5640b881-6c2f-5b43-a3d8-b62d4845671").failed());
    CHECK(uuidFromString("5640b881-6c2f-5b43-a3d8-b62d4845671fx").failed());
    CHECK(uuidFromString("5640b881-6c2f-5b43-a3d8-b62d4845671g").failed());
    CHECK(uuidFromString("").failed());
    CHECK(uuidFromString("5640B881-6C2F-5B43-A3D8-B62D4845671F").ok()); // uppercase accepted
}

TEST_CASE("Board ID length validation", "[protocol][uuid]") {
    const std::array<std::uint8_t, 7> tooShort = {1, 2, 3, 4, 5, 6, 7};
    CHECK(deviceUuidFromBoardId(view(tooShort)).failed());
}

TEST_CASE("isZero and ordering", "[protocol][uuid]") {
    Uuid zero{};
    Uuid other = deviceUuidFromBoardIdU64(1);
    CHECK(zero.isZero());
    CHECK_FALSE(other.isZero());
    CHECK(zero < other);
    CHECK(other == deviceUuidFromBoardIdU64(1));
}
