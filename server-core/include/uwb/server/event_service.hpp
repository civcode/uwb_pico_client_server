#pragma once

#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

#include "uwb/protocol/events.hpp"
#include "uwb/server/clock.hpp"
#include "uwb/server/server_errors.hpp"
#include "uwb/server/server_config.hpp"
#include "uwb/server/transport.hpp"
#include "uwb/server/tx_queue.hpp"

namespace uwb::server {

using uwb::protocol::EventId;
using uwb::protocol::StreamMode;
using uwb::protocol::StreamState;
using uwb::protocol::StreamStatusReason;

// One Event Control subscription / stream (specification §31, §36, §37).
struct StreamInfo {
    std::uint16_t streamId = 0;
    EventId eventId = EventId::UwbMeasurement;
    StreamMode mode = StreamMode::Live;
    std::uint16_t periodMs = 0;
    StreamState state = StreamState::Active;
    std::uint32_t droppedCount = 0;
    std::uint32_t sequence = 0;      // per-stream sequence, starts at 1 (§32)
    std::uint64_t lastEmittedUs = 0; // rate limiting (§31.3)
};

class EventService {
public:
    EventService(const ServerConfig &config, const IClock &clock) noexcept;

    // Subscribe (specification §31.1 sub-function 0x01). streamId is assigned by
    // the server and is unique within one TCP connection (§31.4).
    [[nodiscard]] ServerResult<StreamInfo> subscribe(ConnectionId connection, EventId event, StreamMode mode,
                                                     std::uint16_t requestedPeriodMs);

    [[nodiscard]] ServerResult<StreamInfo> unsubscribe(ConnectionId connection, std::uint16_t streamId);
    [[nodiscard]] ServerResult<StreamInfo> query(ConnectionId connection, std::uint16_t streamId) const;
    [[nodiscard]] std::uint16_t unsubscribeAll(ConnectionId connection);

    void connectionClosed(ConnectionId connection) noexcept;

    // Producer side. `payload` is the encoded event payload (§34..§36). Returns the
    // number of subscriptions that accepted the event.
    std::size_t publish(EventId event, uwb::protocol::ConstBytes payload);

    // Move queued stream events into the connection's TX queue (High priority for
    // measurements/position/stream status per specification §38).
    std::size_t flush(ConnectionId connection, ConnectionTxQueue &tx, std::uint64_t nowUs);

    [[nodiscard]] std::size_t streamCount(ConnectionId connection) const noexcept;
    [[nodiscard]] std::uint32_t totalDropped(ConnectionId connection) const noexcept;

    // Effective per-stream queue capacity (§53).
    [[nodiscard]] std::uint32_t queueCapacity(StreamMode mode) const noexcept;

private:
    struct QueuedEvent {
        EventId eventId = EventId::UwbMeasurement;
        uwb::protocol::ByteBuffer payload;
    };

    struct Stream {
        StreamInfo info;
        std::deque<QueuedEvent> queue;
        ConnectionId connection = kInvalidConnectionId;
    };

    [[nodiscard]] Stream *findStream(ConnectionId connection, std::uint16_t streamId) noexcept;
    [[nodiscard]] const Stream *findStream(ConnectionId connection, std::uint16_t streamId) const noexcept;

    void queueStreamStatus(Stream &stream, StreamState state, StreamStatusReason reason);

    const ServerConfig &config_;
    const IClock &clock_;
    std::uint16_t nextStreamId_ = 1;
    std::unordered_map<ConnectionId, std::vector<Stream>> streams_;
};

} // namespace uwb::server
