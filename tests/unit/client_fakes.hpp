#pragma once

// Scripted collaborators for client-core unit tests (implementation plan §28).
//
// The client core is transport agnostic, so these fakes replace the Asio
// adapters completely: no sockets are involved in unit tests.

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "uwb/client/client_clock.hpp"
#include "uwb/client/client_connection.hpp"
#include "uwb/client/client_transport.hpp"
#include "uwb/client/discovery_client.hpp"
#include "uwb/protocol/payloads.hpp"
#include "uwb/protocol/wire_writer.hpp"

namespace uwb::client::test {

using namespace uwb::protocol;

// ----------------------------------------------------------------------------
// Clock and executor
// ----------------------------------------------------------------------------

class ManualClientClock final : public IClock {
public:
    [[nodiscard]] std::uint64_t nowUs() const noexcept override { return nowUs_; }
    void setUs(std::uint64_t value) noexcept { nowUs_ = value; }
    void advanceMs(std::uint64_t milliseconds) noexcept { nowUs_ += milliseconds * 1000ULL; }
    void advanceUs(std::uint64_t microseconds) noexcept { nowUs_ += microseconds; }

private:
    std::uint64_t nowUs_ = 1'000'000ULL;
};

// Unit tests model the single I/O thread by running posted work inline.
class InlineExecutor final : public ITaskExecutor {
public:
    void post(std::function<void()> task) override {
        ++posts_;
        if (task) {
            task();
        }
    }
    [[nodiscard]] std::uint64_t posts() const noexcept { return posts_; }

private:
    std::uint64_t posts_ = 0;
};

// ----------------------------------------------------------------------------
// Scripted TCP transport
// ----------------------------------------------------------------------------

class ScriptedTransport final : public IClientTransport {
public:
    explicit ScriptedTransport(ClientConnection &owner) noexcept : owner_(&owner) {}

    void connect(std::uint32_t timeoutMs,
                 std::function<void(TransportStatus, const std::string &)> onDone) override {
        connectCalls_++;
        lastTimeoutMs_ = timeoutMs;
        onDone_ = std::move(onDone);
    }

    void write(ConstBytes frame) override {
        writes_.push_back(ByteBuffer(frame.begin(), frame.end()));
    }

    void close() override { open_ = false; }
    [[nodiscard]] bool isOpen() const noexcept override { return open_; }

    // Simulates a transport that currently cannot accept data (socket buffer
    // full): the connection must queue frames and apply its own limit.
    void pauseWrites() noexcept { open_ = false; }
    void resumeWrites() noexcept { open_ = true; }

    // ---- scripted device behaviour ------------------------------------------
    // Adapter contract: the connect callback reports success and the adapter
    // then delivers onConnected() to the owning connection.
    void acceptConnect() {
        open_ = true;
        if (onDone_) {
            auto callback = std::move(onDone_);
            callback(TransportStatus::Ok, std::string{});
        }
        owner_->onConnected();
    }

    void rejectConnect(const std::string &message) {
        open_ = false;
        if (onDone_) {
            auto callback = std::move(onDone_);
            callback(TransportStatus::ConnectFailed, message);
        }
    }

    void drop(TransportStatus status = TransportStatus::Closed, const std::string &message = "peer closed") {
        open_ = false;
        owner_->onClosed(status, message);
    }

    void deliver(ConstBytes bytes) { owner_->onBytes(bytes); }
    void deliverFrame(const ByteBuffer &frame) { owner_->onBytes(ConstBytes{frame.data(), frame.size()}); }

    [[nodiscard]] const std::vector<ByteBuffer> &writes() const noexcept { return writes_; }
    [[nodiscard]] std::size_t connectCalls() const noexcept { return connectCalls_; }
    [[nodiscard]] std::uint32_t lastConnectTimeoutMs() const noexcept { return lastTimeoutMs_; }

private:
    ClientConnection *owner_;
    std::function<void(TransportStatus, const std::string &)> onDone_;
    std::vector<ByteBuffer> writes_;
    bool open_ = false;
    std::size_t connectCalls_ = 0;
    std::uint32_t lastTimeoutMs_ = 0;
};

// Factory that hands out ScriptedTransport objects and remembers them.
class ScriptedTransportFactory final : public IClientTransportFactory {
public:
    struct Leak {
        ScriptedTransport *transport = nullptr;
        Endpoint endpoint;
    };

    [[nodiscard]] std::unique_ptr<IClientTransport> create(const Endpoint &endpoint, ClientConnection &owner) override {
        auto transport = std::make_unique<ScriptedTransport>(owner);
        ScriptedTransport *raw = transport.get();
        transports_.push_back(ScriptedTransportLease{raw, endpoint});
        return transport;
    }

    struct ScriptedTransportLease {
        ScriptedTransport *transport = nullptr;
        Endpoint endpoint;
    };

    [[nodiscard]] const std::vector<ScriptedTransportLease> &transports() const noexcept { return transports_; }
    [[nodiscard]] std::size_t createdCount() const noexcept { return transports_.size(); }
    [[nodiscard]] ScriptedTransport *last() noexcept {
        return transports_.empty() ? nullptr : transports_.back().transport;
    }

private:
    std::vector<ScriptedTransportLease> transports_;
};

// ----------------------------------------------------------------------------
// Scripted UDP discovery transport
// ----------------------------------------------------------------------------

class ScriptedDiscoveryTransport final : public IDiscoveryTransport {
public:
    struct Send {
        Endpoint target;
        ByteBuffer datagram;
    };

    void setHandler(std::function<void(const Endpoint &, ConstBytes)> handler) override {
        handler_ = std::move(handler);
    }
    void send(const Endpoint &target, ConstBytes datagram) override {
        sends_.push_back(Send{target, ByteBuffer(datagram.begin(), datagram.end())});
    }
    [[nodiscard]] bool isOpen() const noexcept override { return open_; }
    void open() noexcept { open_ = true; }

    void deliver(const Endpoint &source, const ByteBuffer &datagram) {
        if (handler_) {
            handler_(source, ConstBytes{datagram.data(), datagram.size()});
        }
    }

    [[nodiscard]] const std::vector<Send> &sends() const noexcept { return sends_; }

private:
    std::function<void(const Endpoint &, ConstBytes)> handler_;
    std::vector<Send> sends_;
    bool open_ = true;
};

// ----------------------------------------------------------------------------
// Frame helpers for simulated devices
// ----------------------------------------------------------------------------

inline ByteBuffer frameOf(PayloadType type, ConstBytes body) {
    Result<ByteBuffer> encoded = encodeFrame(type, body);
    return encoded.ok() ? encoded.value() : ByteBuffer{};
}

inline ByteBuffer activationResponseFrame(const ConnectionActivationResponse &response) {
    const ByteBuffer body = encodeConnectionActivationResponse(response);
    return frameOf(PayloadType::ConnectionActivationResponse, bytesOf(body));
}

inline ByteBuffer applicationFrameOf(const ApplicationEnvelope &envelope) {
    const ByteBuffer body = encodeApplicationEnvelope(envelope);
    return frameOf(PayloadType::ApplicationMessage, bytesOf(body));
}

inline ApplicationEnvelope deviceEnvelope(RequestId transactionId) {
    ApplicationEnvelope envelope;
    envelope.sourceLogicalAddress = 0x1000;
    envelope.targetLogicalAddress = 0x0E01;
    envelope.transactionId = transactionId;
    envelope.flags = 0;
    return envelope;
}

inline ByteBuffer readDidResponseFrame(RequestId transactionId, std::uint16_t did, ConstBytes data) {
    ByteBuffer pdu;
    WireWriter writer(pdu);
    writer.writeU8(static_cast<std::uint8_t>(ServiceId::ReadDataByIdentifier));
    writer.writeU16(did);
    writer.writeBytes(data);
    ApplicationEnvelope envelope = deviceEnvelope(transactionId);
    envelope.servicePdu = std::move(pdu);
    return applicationFrameOf(envelope);
}

inline ByteBuffer negativeResponseFrame(RequestId transactionId, std::uint8_t sid, ServiceNrc nrc) {
    ByteBuffer pdu;
    WireWriter writer(pdu);
    writer.writeU8(0x7F);
    writer.writeU8(sid);
    writer.writeU8(static_cast<std::uint8_t>(nrc));
    ApplicationEnvelope envelope = deviceEnvelope(transactionId);
    envelope.servicePdu = std::move(pdu);
    return applicationFrameOf(envelope);
}

inline ByteBuffer pendingResponseFrame(RequestId transactionId, std::uint8_t sid) {
    return negativeResponseFrame(transactionId, sid, ServiceNrc::ResponsePending);
}

// §21.3: ResponsePending is a negative response PDU 0x7F sid 0x78.

inline ByteBuffer eventNotificationFrame(const EventNotification &event) {
    const ByteBuffer body = encodeEventNotification(event);
    return frameOf(PayloadType::EventNotification, bytesOf(body));
}

inline ByteBuffer aliveCheckResponseFrame(std::uint64_t nonce) {
    AliveCheckResponse response;
    response.nonce = nonce;
    const ByteBuffer body = encodeAliveCheckResponse(response);
    return frameOf(PayloadType::AliveCheckResponse, bytesOf(body));
}

inline ByteBuffer deviceIdResponseFrame(const DeviceIdResponse &identity) {
    const ByteBuffer body = encodeDeviceIdResponse(identity);
    return frameOf(PayloadType::DeviceIdResponse, bytesOf(body));
}

// Find the first recorded frame with the given payload type.
inline std::optional<Frame> firstWriteOfType(const ScriptedTransport &transport, PayloadType type) {
    for (const ByteBuffer &written : transport.writes()) {
        FrameParser parser;
        static_cast<void>(parser.push(ConstBytes{written.data(), written.size()}));
        for (;;) {
            Result<Frame> frame = parser.popFrame();
            if (!frame.ok()) {
                break;
            }
            if (frame.value().type() == type) {
                return frame.value();
            }
        }
    }
    return std::nullopt;
}

// Counts the frames of one payload type across everything the connection wrote.
inline std::size_t countWritesOfType(const ScriptedTransport &transport, PayloadType type) {
    std::size_t total = 0;
    for (const ByteBuffer &written : transport.writes()) {
        FrameParser parser;
        static_cast<void>(parser.push(ConstBytes{written.data(), written.size()}));
        for (;;) {
            Result<Frame> frame = parser.popFrame();
            if (!frame.ok()) {
                break;
            }
            if (frame.value().type() == type) {
                ++total;
            }
        }
    }
    return total;
}

inline ServiceRequest readDidRequest(std::uint16_t did) {
    ServiceRequest request;
    request.sid = static_cast<std::uint8_t>(ServiceId::ReadDataByIdentifier);
    ByteBuffer pdu;
    WireWriter writer(pdu);
    writer.writeU8(static_cast<std::uint8_t>(ServiceId::ReadDataByIdentifier));
    writer.writeU16(did);
    request.pdu = std::move(pdu);
    return request;
}

inline ConnectionOptions testConnectionOptions() {
    ConnectionOptions options;
    options.deviceUuid = domain::DeviceUuid{};
    options.clientInstanceUuid = domain::DeviceUuid{};
    options.timeouts.connectTimeoutMs = 500;
    options.timeouts.requestTimeoutMs = 1000;
    options.timeouts.aliveCheckIdleMs = 1000;
    options.timeouts.clientPresentIntervalMs = 2000;
    options.timeouts.reconnectInitialDelayMs = 100;
    options.timeouts.reconnectMaxDelayMs = 1000;
    return options;
}

} // namespace uwb::client::test
