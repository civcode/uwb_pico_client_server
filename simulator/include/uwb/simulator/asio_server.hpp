#pragma once

#include <asio.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>

#include "uwb/protocol/uuid.hpp"
#include "uwb/server/transport.hpp"
#include "uwb/simulator/simulator_runtime.hpp"

namespace uwb::simulator {

// Asio transport adapter for the simulator (implementation plan §20, §21).
//
//   UDP 13401  Device discovery request/response (specification §8, §17)
//   TCP 13401  framed protocol connections (specification §13, §18)
//
// All socket work happens on one thread. Outbound frames are queued per
// connection and written asynchronously, so a stalled or unreadable client can
// never block the protocol loop (specification §38); the queue is bounded and
// IConnectionWriter::writeFrame() reports refusal so ServerCore keeps the frames
// in its own priority queues.
class SimulatorServer {
public:
    static constexpr std::size_t kMaxQueuedFrames = 64;
    static constexpr std::uint32_t kTickIntervalMs = kSimulatorTickIntervalMs;

    SimulatorServer(asio::io_context &io, SimulatorRuntime &runtime, const SimulatorOptions &options);
    ~SimulatorServer();

    // Binds UDP and TCP. Returns false and fills 'error' when a bind fails.
    bool start(std::string &error);

    // Blocks serving sockets and the protocol tick until requestStop() is called.
    void run();

    // One non-blocking io_context iteration (used by single-threaded harnesses).
    void poll();

    // Safe from any thread: cancels the pending socket work and makes run() return.
    // It deliberately does not touch the connection bookkeeping, which belongs to
    // the event-loop thread.
    void requestStop() noexcept;

    // Full teardown. Only valid once no event-loop thread is running (join it
    // first), otherwise the loop thread would mutate the connection map
    // concurrently.
    void stop() noexcept;

    [[nodiscard]] std::uint16_t udpPort() const noexcept;
    [[nodiscard]] std::uint16_t tcpPort() const noexcept;
    [[nodiscard]] std::size_t connectionCount() const noexcept { return connections_.size(); }

    // Closed by the core (specification §18.4) or by a transport error.
    void closeConnection(uwb::server::ConnectionId id) noexcept;

private:
    // Connections are shared because an outstanding async_read_some / async_write
    // completion keeps a reference alive; erasing the map entry must not free the
    // object while such a completion is still pending.
    struct TcpConnection final : uwb::server::IConnectionWriter,
                                 public std::enable_shared_from_this<TcpConnection> {
        TcpConnection(SimulatorServer &server, asio::io_context &context, std::uint32_t connectionId)
            : owner(server), io(context), id(connectionId), socket(context) {}

        bool writeFrame(uwb::protocol::ConstBytes frame) override;
        [[nodiscard]] bool isOpen() const noexcept override { return open && !closing; }

        SimulatorServer &owner;
        asio::io_context &io;
        std::uint32_t id = 0;
        asio::ip::tcp::socket socket;
        std::array<std::uint8_t, 4096> readBuffer{};
        std::deque<uwb::protocol::ByteBuffer> out;
        uwb::protocol::ByteBuffer inFlight;
        bool writing = false;
        bool open = false;
        bool closing = false; // no new socket operations may be armed
    };

    using ConnectionPtr = std::shared_ptr<TcpConnection>;

    void startAccept();
    void onAccepted(std::error_code ec, asio::ip::tcp::socket socket);
    void startRead(const ConnectionPtr &connection);
    void onRead(const ConnectionPtr &connection, std::error_code ec, std::size_t bytesRead);
    void startNextWrite(const ConnectionPtr &connection);
    void onWritten(const ConnectionPtr &connection, std::error_code ec);
    void startDiscoveryRead();
    void onDiscoveryDatagram(std::error_code ec, std::size_t size, asio::ip::udp::endpoint sender);
    void scheduleTick();
    void onTick(std::error_code ec);
    void closeIdleConnections();
    void teardownConnections() noexcept;

    asio::io_context &io_;
    SimulatorRuntime &runtime_;
    SimulatorOptions options_;

    asio::ip::udp::socket udpSocket_;
    asio::ip::tcp::acceptor acceptor_;
    asio::steady_timer tickTimer_;

    asio::ip::udp::endpoint senderEndpoint_;

    std::unordered_map<uwb::server::ConnectionId, ConnectionPtr> connections_;
    uwb::server::ConnectionId nextConnectionId_ = 1;
    std::atomic<bool> stopping_{false};
    std::array<std::uint8_t, 2048> udpBuffer_{};
    bool running_ = false;
};

} // namespace uwb::simulator
