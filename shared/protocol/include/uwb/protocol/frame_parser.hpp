#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

#include "uwb/protocol/bytes.hpp"
#include "uwb/protocol/constants.hpp"
#include "uwb/protocol/generic_header.hpp"
#include "uwb/protocol/result.hpp"

namespace uwb::protocol {

// Incremental TCP frame parser (specification §13).
//
// Feed arbitrary transport chunks; complete frames are buffered internally and
// retrieved with popFrame(). Handles partial headers, partial payloads, several
// frames in one chunk, and a frame plus the beginning of the next one.
//
// A payload type that is not defined in v1 is surfaced as a frame with the raw
// type value so that the server can answer with GenericHeaderNack
// UnknownPayloadType and keep the connection open. Version and size violations
// are severe: they latch the parser into a failed state and map to
// NACK + connection close.
class FrameParser {
public:
    explicit FrameParser(std::uint32_t maxPayload = kMaxProtocolPayload) noexcept
        : maxPayload_(maxPayload) {}

    void reset() noexcept;

    // Deliver transport bytes. Returns the number of complete frames available.
    [[nodiscard]] Result<std::size_t> push(ConstBytes chunk);

    // Retrieve the next complete frame, or NeedMoreData when none is available.
    [[nodiscard]] Result<Frame> popFrame();

    [[nodiscard]] std::size_t bufferedFrames() const noexcept { return frames_.size(); }
    [[nodiscard]] std::size_t pendingBytes() const noexcept { return buffer_.size(); }
    [[nodiscard]] bool failed() const noexcept { return error_.has_value(); }
    [[nodiscard]] std::optional<ProtocolError> error() const noexcept { return error_; }
    [[nodiscard]] std::uint32_t maxPayload() const noexcept { return maxPayload_; }

private:
    void parseAvailable();
    void fail(ProtocolError error);

    ByteBuffer buffer_;
    std::deque<Frame> frames_;
    std::optional<ProtocolError> error_;
    std::uint32_t maxPayload_;
};

} // namespace uwb::protocol
