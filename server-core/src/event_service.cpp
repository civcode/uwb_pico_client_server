#include "uwb/server/event_service.hpp"

#include <algorithm>

#include "uwb/protocol/events.hpp"
#include "uwb/protocol/generic_header.hpp"
#include "uwb/protocol/payload_types.hpp"

namespace uwb::server {

using uwb::protocol::ByteBuffer;
using uwb::protocol::ConstBytes;
using uwb::protocol::EventNotification;
using uwb::protocol::bytesOf;
using uwb::protocol::PayloadType;

EventService::EventService(const ServerConfig &config, const IClock &clock) noexcept
    : config_(config), clock_(clock) {}

std::uint32_t EventService::queueCapacity(StreamMode mode) const noexcept {
    return mode == StreamMode::Live ? config_.queues.liveStreamQueue : config_.queues.recordingStreamQueue;
}

EventService::Stream *EventService::findStream(ConnectionId connection, std::uint16_t streamId) noexcept {
    auto it = streams_.find(connection);
    if (it == streams_.end()) {
        return nullptr;
    }
    for (auto &stream : it->second) {
        if (stream.info.streamId == streamId) {
            return &stream;
        }
    }
    return nullptr;
}

const EventService::Stream *EventService::findStream(ConnectionId connection, std::uint16_t streamId) const noexcept {
    auto it = streams_.find(connection);
    if (it == streams_.end()) {
        return nullptr;
    }
    for (const auto &stream : it->second) {
        if (stream.info.streamId == streamId) {
            return &stream;
        }
    }
    return nullptr;
}

ServerResult<StreamInfo> EventService::subscribe(ConnectionId connection, EventId event, StreamMode mode,
                                           std::uint16_t requestedPeriodMs) {
    auto &list = streams_[connection];
    if (list.size() >= static_cast<std::size_t>(config_.maxStreamsPerConnection)) {
        return ServerResult<StreamInfo>::err(ServerStatus::TooManyStreams); // §31.4
    }

    // Server-side rate clamp: a client cannot request a faster rate than the
    // configured maximum events per second (specification §31.3).
    std::uint16_t periodMs = requestedPeriodMs;
    if (periodMs > 0 && config_.maxEventsPerSecond > 0) {
        const std::uint32_t minPeriod = 1000U / config_.maxEventsPerSecond;
        if (periodMs < minPeriod) {
            periodMs = static_cast<std::uint16_t>(minPeriod);
        }
    }

    Stream stream;
    stream.connection = connection;
    stream.info.streamId = nextStreamId_++;
    if (nextStreamId_ == 0) {
        nextStreamId_ = 1; // streamId is non-zero and unique per connection (§31.4)
    }
    stream.info.eventId = event;
    stream.info.mode = mode;
    stream.info.periodMs = periodMs;
    stream.info.state = StreamState::Active;
    // Rate limiting measures the gap since the last emitted sample; the first
    // sample after a subscribe is never suppressed (§31.3).
    stream.info.lastEmittedUs = 0;
    list.push_back(std::move(stream));

    return ServerResult<StreamInfo>::okResult(list.back().info);
}

ServerResult<StreamInfo> EventService::unsubscribe(ConnectionId connection, std::uint16_t streamId) {
    const auto *stream = findStream(connection, streamId);
    if (stream == nullptr) {
        return ServerResult<StreamInfo>::err(ServerStatus::UnknownStream);
    }

    StreamInfo info = stream->info;
    info.state = StreamState::Stopped;

    auto &list = streams_[connection];
    list.erase(std::remove_if(list.begin(), list.end(),
                              [streamId](const Stream &s) { return s.info.streamId == streamId; }),
               list.end());
    if (list.empty()) {
        streams_.erase(connection);
    }
    return ServerResult<StreamInfo>::okResult(info);
}

ServerResult<StreamInfo> EventService::query(ConnectionId connection, std::uint16_t streamId) const {
    const auto *stream = findStream(connection, streamId);
    if (stream == nullptr) {
        return ServerResult<StreamInfo>::err(ServerStatus::UnknownStream);
    }
    return ServerResult<StreamInfo>::okResult(stream->info);
}

std::uint16_t EventService::unsubscribeAll(ConnectionId connection) {
    auto it = streams_.find(connection);
    if (it == streams_.end()) {
        return 0;
    }
    const std::uint16_t count = static_cast<std::uint16_t>(it->second.size());
    streams_.erase(it);
    return count;
}

void EventService::connectionClosed(ConnectionId connection) noexcept {
    streams_.erase(connection);
}

void EventService::queueStreamStatus(Stream &stream, StreamState state, StreamStatusReason reason) {
    uwb::protocol::StreamStatusEvent status;
    status.streamId = stream.info.streamId;
    status.state = static_cast<std::uint8_t>(state);
    status.reason = static_cast<std::uint8_t>(reason);
    status.droppedCount = stream.info.droppedCount;

    QueuedEvent item;
    item.eventId = EventId::StreamStatus;
    item.payload = uwb::protocol::encodeStreamStatusEvent(status);
    stream.queue.push_back(std::move(item));
}

std::size_t EventService::publish(EventId event, ConstBytes payload) {
    const std::uint64_t nowUs = clock_.monotonicUs();
    std::size_t accepted = 0;

    for (auto &[connection, list] : streams_) {
        (void)connection;
        for (auto &stream : list) {
            if (stream.info.eventId != event || stream.info.state != StreamState::Active) {
                continue;
            }

            // Rate limiting: samples arriving faster than the agreed period are
            // skipped rather than queued (specification §31.3).
            if (stream.info.periodMs > 0 && nowUs - stream.info.lastEmittedUs < msToUs(stream.info.periodMs)) {
                continue;
            }

            const std::uint32_t capacity = queueCapacity(stream.info.mode);
            if (stream.queue.size() >= capacity) {
                if (stream.info.mode == StreamMode::Live) {
                    // Live: drop the oldest sample, keep streaming, count it (§37).
                    stream.queue.pop_front();
                    ++stream.info.droppedCount;
                } else {
                    // Recording: never silently discard; fail the stream visibly (§37).
                    stream.info.state = StreamState::Failed;
                    queueStreamStatus(stream, StreamState::Failed, StreamStatusReason::RecordingQueueOverflow);
                    continue;
                }
            }

            QueuedEvent item;
            item.eventId = event;
            item.payload.assign(payload.begin(), payload.end());
            stream.queue.push_back(std::move(item));
            stream.info.lastEmittedUs = nowUs;
            ++accepted;
        }
    }
    return accepted;
}

namespace {
// §38 transmission classes: Stream Status is high priority, non-critical status
// events are normal priority, and measurement/position/sensor samples are
// streaming traffic that is drained last.
[[nodiscard]] TxPriority priorityForEvent(EventId id) noexcept {
    switch (id) {
    case EventId::StreamStatus:
        return TxPriority::High;
    case EventId::UwbStatus:
    case EventId::DiagnosticLog:
        return TxPriority::Normal;
    case EventId::UwbMeasurement:
    case EventId::UwbLocalPosition:
    case EventId::SensorData:
        break;
    }
    return TxPriority::Low;
}
} // namespace

std::size_t EventService::flush(ConnectionId connection, ConnectionTxQueue &tx, std::uint64_t nowUs) {
    auto listIt = streams_.find(connection);
    if (listIt == streams_.end()) {
        return 0;
    }

    std::size_t queued = 0;
    for (auto &stream : listIt->second) {
        while (!stream.queue.empty()) {
            QueuedEvent item = std::move(stream.queue.front());
            stream.queue.pop_front();

            EventNotification note;
            note.eventId = static_cast<std::uint16_t>(item.eventId);
            note.streamId = stream.info.streamId;
            note.sequence = ++stream.info.sequence;
            note.timestampUs = nowUs;
            note.payload = std::move(item.payload);

            const ByteBuffer envelope = uwb::protocol::encodeEventNotification(note);
            auto frame = uwb::protocol::encodeFrame(PayloadType::EventNotification, uwb::protocol::bytesOf(envelope));
            if (!frame.ok()) {
                continue; // cannot happen for v1 event sizes; drop rather than emit garbage
            }
            // §38: Stream Status is high priority; range/angle/localization
            // samples use the streaming class so a response never waits behind
            // them. If the connection cannot accept a sample, it is counted as
            // dropped instead of being lost silently (§37).
            const PushOutcome outcome = tx.push(bytesOf(frame.value()), priorityForEvent(item.eventId));
            if (outcome == PushOutcome::Accepted) {
                ++queued;
            } else {
                ++stream.info.droppedCount;
            }
        }
    }
    return queued;
}

std::size_t EventService::streamCount(ConnectionId connection) const noexcept {
    auto it = streams_.find(connection);
    return it == streams_.end() ? 0 : it->second.size();
}

std::uint32_t EventService::totalDropped(ConnectionId connection) const noexcept {
    auto it = streams_.find(connection);
    if (it == streams_.end()) {
        return 0;
    }
    std::uint32_t total = 0;
    for (const auto &stream : it->second) {
        total += stream.info.droppedCount;
    }
    return total;
}

} // namespace uwb::server
