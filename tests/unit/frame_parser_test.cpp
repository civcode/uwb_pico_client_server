#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "uwb/protocol/frame_parser.hpp"
#include "uwb/protocol/payload_types.hpp"
#include "uwb/protocol/wire_writer.hpp"

#include "gen/golden_vectors.hpp"
#include "test_helpers.hpp"

using namespace uwb::protocol;
using uwb::test::sameBytes;
using uwb::test::view;

namespace {

ByteBuffer rawFrame(std::uint8_t version, std::uint8_t inverse, std::uint16_t type, ConstBytes payload) {
    ByteBuffer out;
    WireWriter w{out};
    w.writeU8(version);
    w.writeU8(inverse);
    w.writeU16(type);
    w.writeU32(static_cast<std::uint32_t>(payload.size()));
    w.writeBytes(payload);
    return out;
}

} // namespace

TEST_CASE("FrameParser accepts a complete frame in one chunk", "[protocol][frame][golden]") {
    FrameParser parser;
    auto count = parser.push(view(uwb::test::kDeviceIdResponseFrame));
    REQUIRE(count.ok());
    CHECK(*count == 1);

    auto frame = parser.popFrame();
    REQUIRE(frame.ok());
    CHECK(frame->type() == PayloadType::DeviceIdResponse);
    CHECK(frame->payloadLength() == 43U);
    CHECK(frame->payload.size() == 43);
    CHECK(parser.popFrame().code() == ProtocolErrorCode::NeedMoreData);
    CHECK(parser.bufferedFrames() == 0);
    CHECK(parser.pendingBytes() == 0);
}

TEST_CASE("FrameParser reassembles byte-at-a-time fragmentation", "[protocol][frame]") {
    ByteBuffer stream;
    const std::array<std::uint8_t, 8> nonce = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    ByteBuffer a = rawFrame(0x10, 0xEF, 0x0007, view(nonce));
    const std::array<std::uint8_t, 8> echo = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    ByteBuffer b = rawFrame(0x10, 0xEF, 0x0008, view(echo));
    stream.insert(stream.end(), a.begin(), a.end());
    stream.insert(stream.end(), b.begin(), b.end());

    FrameParser parser;
    std::vector<Frame> got;
    for (std::size_t i = 0; i < stream.size(); ++i) {
        ConstBytes one{stream.data() + i, 1};
        auto count = parser.push(one);
        REQUIRE(count.ok());
        while (parser.bufferedFrames() != 0) {
            auto frame = parser.popFrame();
            REQUIRE(frame.ok());
            got.push_back(std::move(frame.value()));
        }
    }

    REQUIRE(got.size() == 2);
    CHECK(got[0].type() == PayloadType::AliveCheckRequest);
    CHECK(got[1].type() == PayloadType::AliveCheckResponse);
    CHECK(got[0].payload.front() == 0x01);
    CHECK(got[1].payload.front() == 0x08);
    CHECK(got[1].payload.back() == 0x01);
}

TEST_CASE("FrameParser coalesces several frames in one chunk", "[protocol][frame]") {
    const std::array<std::uint8_t, 2> pdu = {0x3E, 0x00};
    ByteBuffer stream;

    ByteBuffer ping = rawFrame(0x10, 0xEF, 0x8001, view(pdu));
    ByteBuffer pong = rawFrame(0x10, 0xEF, 0x8002, view(pdu));
    stream.insert(stream.end(), ping.begin(), ping.end());
    stream.insert(stream.end(), pong.begin(), pong.end());

    // plus the beginning of a third frame
    const std::array<std::uint8_t, 3> partial = {0x10, 0xEF, 0x00};
    stream.insert(stream.end(), partial.begin(), partial.end());

    FrameParser parser;
    auto count = parser.push(bytesOf(stream));
    REQUIRE(count.ok());
    CHECK(*count == 2);
    CHECK(parser.pendingBytes() == 3);

    CHECK(parser.popFrame()->type() == PayloadType::ApplicationMessage);
    CHECK(parser.popFrame()->type() == PayloadType::ApplicationMessageAck);
    CHECK(parser.popFrame().code() == ProtocolErrorCode::NeedMoreData);

    // The trailing partial header is retained for the next chunk.
    const std::array<std::uint8_t, 5> rest = {0x07, 0x00, 0x00, 0x00, 0x00};
    REQUIRE(parser.push(view(rest)).ok());
    CHECK(parser.popFrame()->type() == PayloadType::AliveCheckRequest);
}

TEST_CASE("FrameParser surfaces unknown payload types instead of failing", "[protocol][frame]") {
    const std::array<std::uint8_t, 1> payload = {0x2A};
    ByteBuffer frame = rawFrame(0x10, 0xEF, 0x9A99, view(payload));
    FrameParser parser;
    REQUIRE(parser.push(bytesOf(frame)).ok());

    auto decoded = parser.popFrame();
    REQUIRE(decoded.ok());
    CHECK(static_cast<std::uint16_t>(decoded->type()) == 0x9A99);
    CHECK_FALSE(isKnownPayloadType(decoded->type()));
    CHECK(decoded->payload.size() == 1);
    CHECK_FALSE(parser.failed()); // server NACKs and keeps the connection open (§13)
}

TEST_CASE("FrameParser rejects oversized frames before buffering the payload", "[protocol][frame]") {
    ByteBuffer out;
    {
        WireWriter w{out};
        w.writeU8(0x10);
        w.writeU8(0xEF);
        w.writeU16(0x8001);
        w.writeU32(kMaxProtocolPayload + 1);
    }

    FrameParser parser;
    auto count = parser.push(bytesOf(out));
    REQUIRE(count.failed());
    CHECK(count.code() == ProtocolErrorCode::PayloadTooLarge);
    CHECK(parser.failed());
    CHECK(parser.pendingBytes() == 0);

    // Once failed the parser stays failed until reset().
    const std::array<std::uint8_t, 1> junk = {0x00};
    CHECK(parser.push(view(junk)).failed());
    parser.reset();
    CHECK_FALSE(parser.failed());
    CHECK(parser.bufferedFrames() == 0);
}

TEST_CASE("FrameParser latches on a broken version pattern", "[protocol][frame]") {
    const std::array<std::uint8_t, 2> payload = {0x7E, 0x00};
    ByteBuffer frame = rawFrame(0x10, 0x11, 0x8002, view(payload));

    FrameParser parser;
    auto count = parser.push(bytesOf(frame));
    REQUIRE(count.failed());
    CHECK(count.code() == ProtocolErrorCode::InverseVersionMismatch);
    REQUIRE(parser.error().has_value());
    CHECK(parser.error()->code == ProtocolErrorCode::InverseVersionMismatch);
}

TEST_CASE("FrameParser honours a smaller maxPayload (activation limit)", "[protocol][frame]") {
    ByteBuffer bigPayload(17, 0x00);

    FrameParser strict{16};
    ByteBuffer out;
    {
        WireWriter w{out};
        w.writeU8(0x10);
        w.writeU8(0xEF);
        w.writeU16(0x8001);
        w.writeU32(17);
        w.writeBytes(bytesOf(bigPayload));
    }
    CHECK(strict.push(bytesOf(out)).failed());

    FrameParser fits{16};
    ByteBuffer okPayload(16, 0x5A);
    ByteBuffer okFrame = rawFrame(0x10, 0xEF, 0x8001, bytesOf(okPayload));
    REQUIRE(fits.push(bytesOf(okFrame)).ok());
    auto frame = fits.popFrame();
    REQUIRE(frame.ok());
    CHECK(frame->payload.size() == 16);
    CHECK(frame->payload.front() == 0x5A);
}

TEST_CASE("FrameParser handles a zero length payload", "[protocol][frame]") {
    ByteBuffer frame = rawFrame(0x10, 0xEF, 0x8001, ConstBytes{});
    FrameParser parser;
    REQUIRE(parser.push(bytesOf(frame)).ok());
    auto decoded = parser.popFrame();
    REQUIRE(decoded.ok());
    CHECK(decoded->payload.empty());
    CHECK(decoded->payloadLength() == 0U);
}

// Plan 11.2: a valid frame is split after every possible byte and each split
// must reconstruct the same frame.
TEST_CASE("FrameParser reconstructs a frame split after every byte", "[protocol][frame][golden]") {
    ByteBuffer stream{uwb::test::kDeviceIdResponseFrame.begin(), uwb::test::kDeviceIdResponseFrame.end()};
    const ByteBuffer extra{uwb::test::kApplicationEnvelopeReadUuidFrame.begin(),
                           uwb::test::kApplicationEnvelopeReadUuidFrame.end()};
    stream.insert(stream.end(), extra.begin(), extra.end());

    for (std::size_t split = 1; split < stream.size(); ++split) {
        ConstBytes chunkA{stream.data(), split};
        ConstBytes chunkB{stream.data() + split, stream.size() - split};

        FrameParser parser;
        std::vector<Frame> frames;

        auto countA = parser.push(chunkA);
        REQUIRE(countA.ok());
        while (parser.bufferedFrames() != 0) {
            auto frame = parser.popFrame();
            REQUIRE(frame.ok());
            frames.push_back(std::move(frame.value()));
        }

        auto countB = parser.push(chunkB);
        REQUIRE(countB.ok());
        while (parser.bufferedFrames() != 0) {
            auto frame = parser.popFrame();
            REQUIRE(frame.ok());
            frames.push_back(std::move(frame.value()));
        }

        INFO("split at byte " << split);
        REQUIRE(frames.size() == 2);
        CHECK(frames[0].payloadLength() == 43U);
        CHECK(sameBytes(frames[0].payload, view(uwb::test::kDeviceIdResponsePayload)));
        CHECK(sameBytes(frames[1].payload, view(uwb::test::kApplicationEnvelopeReadUuid)));
        CHECK(parser.popFrame().code() == ProtocolErrorCode::NeedMoreData);
        CHECK(parser.pendingBytes() == 0);
    }
}
