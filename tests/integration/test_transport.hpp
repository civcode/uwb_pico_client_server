#pragma once

// Test transport for Phase 3 integration tests (implementation plan §22, §23).
//
// These tests exercise the real transport adapters and the real host event loop
// around the same SimulatorRuntime the simulator executable uses. The client side
// here is deliberately throwaway: the maintained client implementation lives in
// client-core/ and is built in Phase 4.

#include <asio.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "uwb/protocol/bytes.hpp"
#include "uwb/protocol/constants.hpp"
#include "uwb/protocol/frame_parser.hpp"
#include "uwb/protocol/generic_header.hpp"
#include "uwb/protocol/payload_types.hpp"
#include "uwb/protocol/payloads.hpp"
#include "uwb/protocol/result.hpp"
#include "uwb/protocol/service_ids.hpp"
#include "uwb/protocol/services.hpp"
#include "uwb/simulator/asio_server.hpp"
#include "uwb/simulator/simulator_runtime.hpp"

namespace uwb::test {

using uwb::protocol::ApplicationEnvelope;
using uwb::protocol::ByteBuffer;
using uwb::protocol::ConstBytes;
using uwb::protocol::PayloadType;
using uwb::simulator::SimulatorOptions;
using uwb::simulator::SimulatorRuntime;
using uwb::simulator::SimulatorServer;

inline constexpr std::uint16_t kTestClientAddress = 0x0E01;
inline constexpr std::chrono::milliseconds kDefaultTimeout{3000};

struct WireFrame {
    uwb::protocol::GenericHeader header;
    ByteBuffer payload; // frame payload only, without the generic header
};

[[nodiscard]] inline ByteBuffer applicationFrame(std::uint16_t clientAddress, std::uint16_t serverAddress,
                                                 std::uint32_t transactionId, ConstBytes servicePdu,
                                                 bool ackRequired = true) {
    ApplicationEnvelope envelope;
    envelope.sourceLogicalAddress = clientAddress;
    envelope.targetLogicalAddress = serverAddress;
    envelope.transactionId = transactionId;
    envelope.flags = ackRequired ? uwb::protocol::kApplicationFlagAckRequired : 0;
    envelope.servicePdu.assign(servicePdu.begin(), servicePdu.end());
    const ByteBuffer message = uwb::protocol::encodeApplicationEnvelope(envelope);
    auto frame = uwb::protocol::encodeFrame(PayloadType::ApplicationMessage, uwb::protocol::bytesOf(message));
    return frame.ok() ? frame.value() : ByteBuffer{};
}

[[nodiscard]] inline ConstBytes frameBody(const WireFrame &frame) {
    return ConstBytes{frame.payload.data(), frame.payload.size()};
}

[[nodiscard]] inline ByteBuffer servicePduOf(const WireFrame &frame) {
    auto decoded = uwb::protocol::decodeApplicationEnvelope(frameBody(frame));
    return decoded.ok() ? decoded.value().servicePdu : ByteBuffer{};
}

[[nodiscard]] inline std::optional<std::uint32_t> transactionOf(const WireFrame &frame) {
    auto decoded = uwb::protocol::decodeApplicationEnvelope(frameBody(frame));
    if (!decoded.ok()) {
        return std::nullopt;
    }
    return decoded.value().transactionId;
}

// Transport-level negative ACK (0x8003, specification 20.3).
[[nodiscard]] inline std::optional<uwb::protocol::ApplicationMessageNack> applicationNack(const WireFrame &frame) {
    if (frame.header.payloadType != PayloadType::ApplicationMessageNack) {
        return std::nullopt;
    }
    auto decoded = uwb::protocol::decodeApplicationMessageNack(frameBody(frame));
    if (!decoded.ok()) {
        return std::nullopt;
    }
    return decoded.value();
}

[[nodiscard]] inline std::optional<uwb::protocol::ApplicationNackCode> nackOf(const WireFrame &frame) {
    const auto nack = applicationNack(frame);
    if (!nack.has_value()) {
        return std::nullopt;
    }
    return nack->code;
}

[[nodiscard]] inline std::optional<uwb::protocol::ServiceNrc> nrcOf(const WireFrame &frame) {
    if (frame.header.payloadType != PayloadType::ApplicationMessage) {
        return std::nullopt;
    }
    const ByteBuffer pdu = servicePduOf(frame);
    if (pdu.empty() || pdu[0] != uwb::protocol::kNegativeResponseSid) {
        return std::nullopt;
    }
    auto decoded = uwb::protocol::decodeNegativeResponse(uwb::protocol::bytesOf(pdu));
    if (!decoded.ok()) {
        return std::nullopt;
    }
    return decoded.value().second;
}

// Non-blocking Asio TCP client with a FrameParser attached.
class TcpClient {
public:
    // The client owns its Asio context: sharing the simulator's running context
    // would make socket teardown race with the simulator event loop.
    TcpClient(std::uint16_t port, std::uint16_t clientAddress) : address(clientAddress), io(), socket(io) {
        asio::error_code ec;
        socket.connect(asio::ip::tcp::endpoint(asio::ip::address::from_string("127.0.0.1"), port), ec);
        if (ec) {
            throw std::runtime_error("client connect failed: " + ec.message());
        }
        socket.non_blocking(true, ec);
    }

    ~TcpClient() { close(); }

    TcpClient(const TcpClient &) = delete;
    TcpClient &operator=(const TcpClient &) = delete;
    TcpClient(TcpClient &&) = delete;
    TcpClient &operator=(TcpClient &&) = delete;

    void send(const ByteBuffer &frame) {
        std::size_t sent = 0;
        while (sent < frame.size()) {
            asio::error_code ec;
            const asio::const_buffer buffer{frame.data() + sent, frame.size() - sent};
            const std::size_t written = socket.write_some(buffer, ec);
            if (ec == asio::error::would_block) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (ec) {
                throw std::runtime_error("client write failed: " + ec.message());
            }
            sent += written;
        }
    }

    void sendPdu(std::uint32_t transactionId, ConstBytes servicePdu) {
        send(applicationFrame(address, serverAddress, transactionId, servicePdu));
    }

    [[nodiscard]] std::optional<WireFrame> nextFrame(std::chrono::milliseconds timeout = kDefaultTimeout) {
        if (parser.bufferedFrames() == 0 && !pump(timeout)) {
            return std::nullopt;
        }
        auto frame = parser.popFrame();
        if (!frame.ok()) {
            return std::nullopt;
        }
        ConstBytes payload = frame.value().payloadBytes();
        return WireFrame{frame.value().header, ByteBuffer(payload.begin(), payload.end())};
    }

    [[nodiscard]] std::optional<WireFrame> waitFor(std::chrono::milliseconds timeout,
                                                   const std::function<bool(const WireFrame &)> &predicate) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            auto frame = nextFrame(remaining);
            if (!frame) {
                return std::nullopt;
            }
            if (predicate(*frame)) {
                return frame;
            }
        }
        return std::nullopt;
    }

    // Waits for the final answer to a transaction. An NRC 0x78 'responsePending'
    // interim (§21.4) is skipped the same way client-core will skip it in Phase 4.
    [[nodiscard]] std::optional<WireFrame> waitForResponse(std::uint32_t transactionId,
                                                           std::chrono::milliseconds timeout = kDefaultTimeout) {
        return waitForFinal(transactionId, timeout);
    }

    [[nodiscard]] std::optional<WireFrame> waitForFinal(std::uint32_t transactionId,
                                                        std::chrono::milliseconds timeout) {
        return waitFor(timeout, [transactionId](const WireFrame &frame) {
            return frame.header.payloadType == PayloadType::ApplicationMessage &&
                   transactionOf(frame) == transactionId &&
                   nrcOf(frame) != uwb::protocol::ServiceNrc::ResponsePending;
        });
    }

    // Waits for the interim 0x78 answer of an asynchronous request.
    [[nodiscard]] std::optional<WireFrame> waitForPending(std::uint32_t transactionId,
                                                          std::chrono::milliseconds timeout = kDefaultTimeout) {
        return waitFor(timeout, [transactionId](const WireFrame &frame) {
            return frame.header.payloadType == PayloadType::ApplicationMessage &&
                   transactionOf(frame) == transactionId &&
                   nrcOf(frame) == uwb::protocol::ServiceNrc::ResponsePending;
        });
    }

    [[nodiscard]] std::vector<WireFrame> collect(std::chrono::milliseconds window) {
        std::vector<WireFrame> frames;
        const auto deadline = std::chrono::steady_clock::now() + window;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            auto frame = nextFrame(remaining);
            if (!frame) {
                break;
            }
            frames.push_back(std::move(*frame));
        }
        return frames;
    }

    [[nodiscard]] std::size_t countType(const std::vector<WireFrame> &frames, PayloadType type) const {
        std::size_t matches = 0;
        for (const WireFrame &frame : frames) {
            if (frame.header.payloadType == type) {
                ++matches;
            }
        }
        return matches;
    }

    // True when the device closed the socket (EOF) within the timeout.
    [[nodiscard]] bool waitForClose(std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            asio::error_code ec;
            const std::size_t n = socket.read_some(asio::buffer(readBuffer), ec);
            if (ec == asio::error::would_block) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (ec) {
                closed = true;
                return true;
            }
            const auto pushed = parser.push(ConstBytes{readBuffer.data(), n});
            if (!pushed.ok()) {
                parser.reset();
            }
        }
        return false;
    }

    void close() {
        if (closed) {
            return;
        }
        closed = true;
        asio::error_code ec;
        socket.close(ec);
    }

    std::uint16_t address = 0;    // this client's logical address
    std::uint16_t serverAddress = 0; // learned from the Activation response
    std::uint32_t txn = 0;        // monotonically increasing transaction id

private:
    bool pump(std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            asio::error_code ec;
            const std::size_t n = socket.read_some(asio::buffer(readBuffer), ec);
            if (ec == asio::error::would_block) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (ec) {
                closed = true;
                return false;
            }
            auto pushed = parser.push(ConstBytes{readBuffer.data(), n});
            if (!pushed.ok()) {
                parser.reset(); // device violated the transport contract
                return false;
            }
            if (parser.bufferedFrames() > 0) {
                return true;
            }
        }
        return parser.bufferedFrames() > 0;
    }

    asio::io_context io;
    asio::ip::tcp::socket socket;
    uwb::protocol::FrameParser parser;
    std::array<std::uint8_t, 4096> readBuffer{};
    bool closed = false;
};

[[nodiscard]] inline std::optional<WireFrame> discover(std::uint16_t udpPort, std::chrono::milliseconds timeout,
                                                       const ByteBuffer &request) {
    asio::io_context context;
    asio::ip::udp::socket socket(context);
    asio::error_code ec;
    socket.open(asio::ip::udp::v4(), ec);
    socket.bind(asio::ip::udp::endpoint(asio::ip::address::from_string("127.0.0.1"), 0), ec);
    socket.non_blocking(true, ec);

    socket.send_to(asio::buffer(request),
                   asio::ip::udp::endpoint(asio::ip::address::from_string("127.0.0.1"), udpPort),
                   static_cast<asio::socket_base::message_flags>(0), ec);
    if (ec) {
        return std::nullopt;
    }

    asio::ip::udp::endpoint sender;
    std::array<std::uint8_t, 2048> buffer{};
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const std::size_t n = socket.receive_from(asio::buffer(buffer), sender, 0, ec);
        if (ec == asio::error::would_block) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (ec) {
            return std::nullopt;
        }
        auto header = uwb::protocol::decodeGenericHeader(ConstBytes{buffer.data(), n});
        if (!header.ok()) {
            return std::nullopt;
        }
        ConstBytes body{buffer.data() + uwb::protocol::kGenericHeaderSize, n - uwb::protocol::kGenericHeaderSize};
        return WireFrame{header.value(), ByteBuffer(body.begin(), body.end())};
    }
    return std::nullopt;
}

inline SimulatorOptions fastOptions() {
    SimulatorOptions options;
    options.measurementPeriodMs = 20; // keeps streaming tests short
    options.verbose = false;
    return options;
}

// Runs the simulator event loop on a worker thread, like the real device would.
class SimulatorFixture {
public:
    explicit SimulatorFixture(SimulatorOptions settings = fastOptions())
        : options(std::move(settings)), deviceAddress(options.logicalAddress), ioContext(), runtime(options),
          server(ioContext, runtime, options) {
        std::string error;
        if (!server.start(error) || server.tcpPort() == 0 || server.udpPort() == 0) {
            throw std::runtime_error("simulator failed to bind its sockets: " + error);
        }
        deviceAddress = options.logicalAddress;
        worker = std::thread([this] { server.run(); });
    }

    ~SimulatorFixture() {
        // Signal first, then join: the loop thread must finish touching the
        // connection bookkeeping before the server tears it down.
        server.requestStop();
        if (worker.joinable()) {
            worker.join();
        }
    }

    SimulatorFixture(const SimulatorFixture &) = delete;
    SimulatorFixture &operator=(const SimulatorFixture &) = delete;

    SimulatorRuntime &runtimeRef() { return runtime; }
    SimulatorServer &serverRef() { return server; }
    asio::io_context &io() { return ioContext; }

    [[nodiscard]] std::uint16_t tcpPort() const { return server.tcpPort(); }
    [[nodiscard]] std::uint16_t udpPort() const { return server.udpPort(); }

    SimulatorOptions options;
    std::uint16_t deviceAddress = 0;

private:
    asio::io_context ioContext;
    SimulatorRuntime runtime;
    SimulatorServer server;
    std::thread worker;
};

[[nodiscard]] inline ByteBuffer discoveryRequestFrame() {
    uwb::protocol::DeviceIdRequest request;
    const ByteBuffer pdu = uwb::protocol::encodeDeviceIdRequest(request);
    auto frame = uwb::protocol::encodeFrame(PayloadType::DeviceIdRequest, uwb::protocol::bytesOf(pdu));
    return frame.ok() ? frame.value() : ByteBuffer{};
}

[[nodiscard]] inline ByteBuffer activationFrame(std::uint16_t clientAddress, uwb::protocol::ConnectionRole role,
                                                std::uint64_t instanceSeed) {
    uwb::protocol::ConnectionActivationRequest request;
    request.clientLogicalAddress = clientAddress;
    request.requestedRole = role;
    request.clientInstanceUuid = uwb::protocol::Uuid{}; // anonymous test client
    request.clientInstanceUuid.bytes[15] = static_cast<std::uint8_t>(instanceSeed & 0xFFU);
    const ByteBuffer pdu = uwb::protocol::encodeConnectionActivationRequest(request);
    auto frame = uwb::protocol::encodeFrame(PayloadType::ConnectionActivationRequest, uwb::protocol::bytesOf(pdu));
    return frame.ok() ? frame.value() : ByteBuffer{};
}

} // namespace uwb::test
