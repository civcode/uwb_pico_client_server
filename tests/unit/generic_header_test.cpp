#include <array>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "uwb/protocol/generic_header.hpp"
#include "uwb/protocol/payload_types.hpp"
#include "uwb/protocol/protocol_version.hpp"
#include "uwb/protocol/wire_writer.hpp"

#include "gen/golden_vectors.hpp"
#include "test_helpers.hpp"

using namespace uwb::protocol;
using uwb::test::sameBytes;
using uwb::test::view;

namespace {

// Raw header bytes, bypassing encodeGenericHeader() so that invalid
// version/inverse combinations can be tested.
ByteBuffer rawHeader(std::uint8_t version, std::uint8_t inverse, std::uint16_t type, std::uint32_t length) {
    ByteBuffer out;
    WireWriter w{out};
    w.writeU8(version);
    w.writeU8(inverse);
    w.writeU16(type);
    w.writeU32(length);
    return out;
}

} // namespace

TEST_CASE("Generic header golden vectors encode byte exact", "[protocol][header][golden]") {
    GenericHeader app{};
    app.protocolVersion = kProtocolVersionV1_0;
    app.payloadType = PayloadType::ApplicationMessage;
    app.payloadLength = 15;
    CHECK(sameBytes(bytesOf(encodeGenericHeader(app)), view(uwb::test::kHeaderApplicationMessage)));

    GenericHeader evt{};
    evt.payloadType = PayloadType::EventNotification;
    evt.payloadLength = 54;
    CHECK(sameBytes(bytesOf(encodeGenericHeader(evt)), view(uwb::test::kHeaderEventNotification)));

    GenericHeader dev{};
    dev.payloadType = PayloadType::DeviceIdResponse;
    dev.payloadLength = 43;
    CHECK(sameBytes(bytesOf(encodeGenericHeader(dev)), view(uwb::test::kHeaderDeviceIdResponse)));
}

TEST_CASE("Generic header decodes version, type and length", "[protocol][header][golden]") {
    auto header = decodeGenericHeader(view(uwb::test::kHeaderDeviceIdResponse));
    REQUIRE(header.ok());
    CHECK(header->protocolVersion == 0x10);
    CHECK(header->payloadType == PayloadType::DeviceIdResponse);
    CHECK(header->payloadLength == 43U);
}

TEST_CASE("Protocol version nibbles (specification §12)", "[protocol][version]") {
    CHECK(protocolMajor(0x12) == 1);
    CHECK(protocolMinor(0x12) == 2);
    CHECK(makeProtocolVersion(1, 0) == 0x10);
    CHECK(inverseProtocolVersion(0x10) == 0xEF);
    CHECK(inverseVersionMatches(0x10, 0xEF));
    CHECK_FALSE(inverseVersionMatches(0x10, 0x0F));
    CHECK(majorCompatible(0x19));
    CHECK_FALSE(majorCompatible(0x20));
    CHECK(sameOrLowerMinor(0x10));
    // Same major, higher minor: framing is accepted, feature support decides.
    CHECK(majorCompatible(0x11));
    CHECK_FALSE(sameOrLowerMinor(0x11));

    // The minor version is ignored during frame validation.
    auto check = validateVersionBytes(0x1F, 0xE0);
    REQUIRE(check.ok());
    CHECK(*check == 0x1F);
}

TEST_CASE("Header validation order is normative (§13)", "[protocol][header]") {
    // 1) version/inverse failure wins over unknown type and oversized length.
    auto badInverse = decodeGenericHeader(bytesOf(rawHeader(0x10, 0x00, 0xFFFF, 0xFFFFFFFF)));
    REQUIRE(badInverse.failed());
    CHECK(badInverse.code() == ProtocolErrorCode::InverseVersionMismatch);

    // 2) correct pattern, unknown type, oversized length: type is checked first.
    auto unknownType = decodeGenericHeader(bytesOf(rawHeader(0x10, 0xEF, 0xFFFF, 0xFFFFFFFF)));
    REQUIRE(unknownType.failed());
    CHECK(unknownType.code() == ProtocolErrorCode::UnknownPayloadType);

    // 3) known type, unsupported major version.
    auto major = decodeGenericHeader(bytesOf(rawHeader(0x20, 0xDF, 0x0004, 1)));
    REQUIRE(major.failed());
    CHECK(major.code() == ProtocolErrorCode::UnsupportedMajorVersion);

    // 4) valid version and type, oversized length.
    auto big = decodeGenericHeader(bytesOf(rawHeader(0x10, 0xEF, 0x8001, 4097)));
    REQUIRE(big.failed());
    CHECK(big.code() == ProtocolErrorCode::PayloadTooLarge);
    CHECK(big.error().detail == 4097U);

    // 5) short header.
    ByteBuffer shortHeader = rawHeader(0x10, 0xEF, 0x8001, 0);
    shortHeader.pop_back();
    auto truncated = decodeGenericHeader(bytesOf(shortHeader));
    REQUIRE(truncated.failed());
    CHECK(truncated.code() == ProtocolErrorCode::HeaderTooShort);
}

TEST_CASE("Maximum payload boundary is inclusive", "[protocol][header]") {
    CHECK(decodeGenericHeader(bytesOf(rawHeader(0x10, 0xEF, 0x8001, kMaxProtocolPayload))).ok());
    CHECK(decodeGenericHeader(bytesOf(rawHeader(0x10, 0xEF, 0x8001, kMaxProtocolPayload + 1))).failed());
}

TEST_CASE("Zero length payload is valid", "[protocol][header]") {
    GenericHeader header{};
    header.payloadType = PayloadType::DeviceIdRequest;
    header.payloadLength = 0;
    auto decoded = decodeGenericHeader(bytesOf(encodeGenericHeader(header)));
    REQUIRE(decoded.ok());
    CHECK(decoded->payloadLength == 0U);
}

TEST_CASE("encodeFrame appends the payload and validates the size", "[protocol][header]") {
    const std::array<std::uint8_t, 3> pdu = {0x22, 0xF0, 0x00};
    auto frame = encodeFrame(PayloadType::ApplicationMessage, view(pdu));
    REQUIRE(frame.ok());
    REQUIRE(frame->size() == kGenericHeaderSize + 3);
    CHECK(sameBytes(ConstBytes{frame->data() + kGenericHeaderSize, frame->size() - kGenericHeaderSize},
                          view(pdu)));

    ByteBuffer huge(kMaxProtocolPayload + 1, 0x00);
    CHECK(encodeFrame(PayloadType::ApplicationMessage, bytesOf(huge)).failed());
}

TEST_CASE("Header errors map to the §15 NACK codes", "[protocol][header]") {
    CHECK(genericHeaderNackFor(ProtocolErrorCode::InverseVersionMismatch) ==
          GenericHeaderNackCode::IncorrectHeaderPattern);
    CHECK(genericHeaderNackFor(ProtocolErrorCode::UnknownPayloadType) == GenericHeaderNackCode::UnknownPayloadType);
    CHECK(genericHeaderNackFor(ProtocolErrorCode::PayloadTooLarge) == GenericHeaderNackCode::MessageTooLarge);
    CHECK(genericHeaderNackFor(ProtocolErrorCode::UnsupportedMajorVersion) ==
          GenericHeaderNackCode::UnsupportedMajorVersion);
    CHECK_FALSE(genericHeaderNackFor(ProtocolErrorCode::NeedMoreData).has_value());
}
