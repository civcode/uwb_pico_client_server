#include "uwb/protocol/frame_parser.hpp"

#include "uwb/protocol/payload_types.hpp"
#include "uwb/protocol/protocol_version.hpp"
#include "uwb/protocol/wire_reader.hpp"

namespace uwb::protocol {

void FrameParser::reset() noexcept {
    buffer_.clear();
    frames_.clear();
    error_.reset();
}

void FrameParser::fail(ProtocolError error) {
    error_ = error;
    buffer_.clear();
    buffer_.shrink_to_fit();
    frames_.clear();
}

Result<std::size_t> FrameParser::push(ConstBytes chunk) {
    if (failed()) {
        return Result<std::size_t>::error(*error_);
    }

    buffer_.insert(buffer_.end(), chunk.begin(), chunk.end());
    parseAvailable();

    if (failed()) {
        return Result<std::size_t>::error(*error_);
    }
    return Result<std::size_t>::ok(frames_.size());
}

Result<Frame> FrameParser::popFrame() {
    if (failed()) {
        return Result<Frame>::error(*error_);
    }
    if (frames_.empty()) {
        return Result<Frame>::error(ProtocolErrorCode::NeedMoreData);
    }

    Frame frame = std::move(frames_.front());
    frames_.pop_front();
    return Result<Frame>::ok(std::move(frame));
}

void FrameParser::parseAvailable() {
    for (;;) {
        if (buffer_.size() < kGenericHeaderSize) {
            return;
        }

        ConstBytes head{buffer_.data(), kGenericHeaderSize};
        WireReader reader{head};

        std::uint8_t version = 0;
        std::uint8_t inverse = 0;
        std::uint16_t type = 0;
        std::uint32_t length = 0;

        if (!reader.readU8(version) || !reader.readU8(inverse) || !reader.readU16(type) ||
            !reader.readU32(length)) {
            fail(ProtocolError{ProtocolErrorCode::HeaderTooShort, 0});
            return;
        }

        if (auto versionCheck = validateVersionBytes(version, inverse); versionCheck.failed()) {
            fail(versionCheck.error());
            return;
        }

        // Reject oversized frames as soon as the header is known, before waiting
        // for or copying the payload (specification §11.3).
        if (length > maxPayload_) {
            fail(ProtocolError{ProtocolErrorCode::PayloadTooLarge, length});
            return;
        }

        const std::size_t frameSize = kGenericHeaderSize + length;
        if (buffer_.size() < frameSize) {
            return; // partial payload
        }

        Frame frame;
        frame.header.protocolVersion = version;
        frame.header.payloadType = static_cast<PayloadType>(type);
        frame.header.payloadLength = length;
        frame.payload.assign(buffer_.begin() + static_cast<std::ptrdiff_t>(kGenericHeaderSize),
                             buffer_.begin() + static_cast<std::ptrdiff_t>(frameSize));

        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(frameSize));
        frames_.push_back(std::move(frame));
    }
}

} // namespace uwb::protocol
