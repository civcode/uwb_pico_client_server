#include <array>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "uwb/protocol/wire_reader.hpp"
#include "uwb/protocol/wire_writer.hpp"

#include "test_helpers.hpp"

using namespace uwb::protocol;
using uwb::test::sameBytes;
using uwb::test::view;

TEST_CASE("WireWriter emits big-endian bytes", "[protocol][wire]") {
    ByteBuffer out;
    WireWriter w{out};
    w.writeU8(0xAB);
    w.writeU16(0x1234);
    w.writeU32(0xDEADBEEF);
    w.writeU64(0x0102030405060708ULL);
    w.writeI16(-2);
    w.writeI32(-1);

    const std::array<std::uint8_t, 21> expected = {
        0xAB, 0x12, 0x34, 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04,
        0x05, 0x06, 0x07, 0x08, 0xFF, 0xFE, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    REQUIRE(out.size() == expected.size());
    CHECK(sameBytes(bytesOf(out), view(expected)));
}

TEST_CASE("WireReader round-trips every scalar width", "[protocol][wire]") {
    ByteBuffer out;
    WireWriter w{out};
    w.writeU8(0x11);
    w.writeU16(0x2222);
    w.writeU32(0x33333333);
    w.writeU64(0x4444444444444444ULL);
    w.writeI16(-300);
    w.writeI32(-70000);
    w.writeI64(-5000000000LL);
    w.writeF32(1.5F);

    WireReader r{bytesOf(out)};
    std::uint8_t u8 = 0;
    std::uint16_t u16 = 0;
    std::uint32_t u32 = 0;
    std::uint64_t u64 = 0;
    std::int16_t i16 = 0;
    std::int32_t i32 = 0;
    std::int64_t i64 = 0;
    float f32 = 0.0F;

    REQUIRE(r.readU8(u8));
    REQUIRE(r.readU16(u16));
    REQUIRE(r.readU32(u32));
    REQUIRE(r.readU64(u64));
    REQUIRE(r.readI16(i16));
    REQUIRE(r.readI32(i32));
    REQUIRE(r.readI64(i64));
    REQUIRE(r.readF32(f32));

    CHECK(u8 == 0x11);
    CHECK(u16 == 0x2222);
    CHECK(u32 == 0x33333333U);
    CHECK(u64 == 0x4444444444444444ULL);
    CHECK(i16 == -300);
    CHECK(i32 == -70000);
    CHECK(i64 == -5000000000LL);
    CHECK(f32 == 1.5F);
    CHECK(r.atEnd());
    CHECK(r.remaining() == 0);
}

TEST_CASE("WireReader fails instead of reading past the end", "[protocol][wire]") {
    const std::array<std::uint8_t, 3> bytes = {0x01, 0x02, 0x03};
    WireReader r{view(bytes)};

    std::uint16_t u16 = 0;
    std::uint32_t u32 = 0;
    std::uint64_t u64 = 0;
    std::array<std::uint8_t, 4> buf{};
    ConstBytes span;

    CHECK(r.readU16(u16));
    CHECK(u16 == 0x0102);
    CHECK(r.remaining() == 1);
    CHECK_FALSE(r.readU16(u16));
    CHECK_FALSE(r.readU32(u32));
    CHECK_FALSE(r.readU64(u64));
    CHECK_FALSE(r.readBytes(buf.data(), 4));
    CHECK_FALSE(r.readSpan(span, 2));
    CHECK_FALSE(r.skip(2));
    CHECK(r.remaining() == 1); // failed reads do not consume
}

TEST_CASE("readBytes copies the correct window", "[protocol][wire]") {
    const std::array<std::uint8_t, 6> bytes = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    WireReader r{view(bytes)};

    std::uint8_t first = 0;
    std::array<std::uint8_t, 3> middle{};
    REQUIRE(r.readU8(first));
    REQUIRE(r.readBytes(middle.data(), 3));
    CHECK(first == 0xAA);
    CHECK((middle == std::array<std::uint8_t, 3>{0xBB, 0xCC, 0xDD}));
    CHECK(r.remaining() == 2);
}

TEST_CASE("readSpan is a zero-copy view of the consumed window", "[protocol][wire]") {
    const std::array<std::uint8_t, 5> bytes = {0x10, 0xEF, 0x80, 0x01, 0x0F};
    WireReader r{view(bytes)};

    ConstBytes span;
    REQUIRE(r.skip(2));
    REQUIRE(r.readSpan(span, 2));
    CHECK(span.data() == bytes.data() + 2); // same storage, no copy
    CHECK(span.size() == 2);
    CHECK((span[0] == 0x80 && span[1] == 0x01));
    CHECK(r.consumed() == 4);
}

TEST_CASE("WireWriter append semantics keep existing content", "[protocol][wire]") {
    ByteBuffer out = {0x00};
    {
        WireWriter w{out};
        w.writeU16(0xBEEF);
    }
    REQUIRE(out.size() == 3);
    CHECK(out[0] == 0x00);
    CHECK(out[1] == 0xBE);
    CHECK(out[2] == 0xEF);
}

TEST_CASE("f32 is transported as IEEE-754 binary32 bits", "[protocol][wire]") {
    ByteBuffer out;
    {
        WireWriter w{out};
        w.writeF32(-0.0F);
    }
    const std::array<std::uint8_t, 4> negativeZero = {0x80, 0x00, 0x00, 0x00};
    REQUIRE(out.size() == negativeZero.size());
    CHECK(sameBytes(bytesOf(out), view(negativeZero)));
}
