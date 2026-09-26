#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <asio/strand.hpp>
#include <asio/steady_timer.hpp>

#include "uwb/client/application_controller.hpp"
#include "uwb/client/client_connection.hpp"
#include "uwb/client/client_transport.hpp"

namespace uwb::client_net {

// Single-threaded executor backed by one io_context (§55: no thread per device).
class AsioExecutor final : public uwb::client::ITaskExecutor {
public:
    using Strand = asio::strand<asio::io_context::executor_type>;

    explicit AsioExecutor(asio::io_context &context);

    void post(std::function<void()> task) override;

    // Blocks until stop() is called from a handler or from another thread.
    void run();
    // Runs whatever is ready without blocking (used by tests).
    std::size_t poll();
    // Runs handlers for at most 'timeout'.  Never call this while another thread
    // is inside run(): one io_context, one owner thread (§55).
    std::size_t runFor(std::chrono::milliseconds timeout);
    void stop();
    [[nodiscard]] bool stopped() const noexcept { return stopped_; }

    [[nodiscard]] asio::io_context &context() noexcept { return context_; }
    [[nodiscard]] Strand strand() { return Strand{context_.get_executor()}; }

private:
    asio::io_context &context_;
    bool stopped_ = false;
};

// Periodic timer that drives ClientConnection::tick and the supervision loops.
class TickTimer final {
public:
    TickTimer(asio::io_context &context, std::chrono::milliseconds interval, std::function<void()> callback);

    void start();
    void stop();
    [[nodiscard]] std::uint64_t ticks() const noexcept { return ticks_; }

private:
    void arm();

    asio::steady_timer timer_;
    std::chrono::milliseconds interval_;
    std::function<void()> callback_;
    bool active_ = false;
    std::uint64_t ticks_ = 0;
};

// TCP endpoint of a client-side endpoint string ("192.168.1.20", port).
[[nodiscard]] asio::ip::tcp::endpoint remoteFrom(const uwb::client::Endpoint &endpoint);

// IClientTransport implementation: one TCP socket, asynchronous, never blocking.
class AsioTcpTransport final : public uwb::client::IClientTransport {
public:
    AsioTcpTransport(asio::io_context &context, AsioExecutor::Strand strand, asio::ip::tcp::endpoint remote,
                     uwb::client::ClientConnection &owner);
    ~AsioTcpTransport() override;

    void connect(std::uint32_t timeoutMs,
                 std::function<void(uwb::client::TransportStatus status, const std::string &message)> onDone) override;
    void write(const uwb::protocol::ConstBytes frame) override;
    void close() override;
    [[nodiscard]] bool isOpen() const noexcept override;

    // Kept behind a shared_ptr so that outstanding asynchronous handlers always
    // see a live object, exactly as the simulator adapter does.
private:
    struct State : public std::enable_shared_from_this<State> {
        asio::ip::tcp::socket socket;
        AsioExecutor::Strand strand;
        asio::steady_timer deadline;
        asio::ip::tcp::endpoint remote;
        uwb::client::ClientConnection &owner;

        std::array<std::uint8_t, 4096> readBuffer{};
        uwb::protocol::ByteBuffer pendingWrite;
        std::function<void(uwb::client::TransportStatus, const std::string &)> connectDone;

        bool connecting = false;
        bool open = false;
        bool closing = false;
        std::string lastError;

        explicit State(asio::io_context &context, AsioExecutor::Strand owningStrand, asio::ip::tcp::endpoint remoteEndpoint,
                       uwb::client::ClientConnection &connection);

        void beginConnect(std::uint32_t timeoutMs);
        void startRead();
        void pump();

        void finish(uwb::client::TransportStatus status, const std::string &message);
    };


    std::shared_ptr<State> state_;
};

class AsioTcpTransportFactory final : public uwb::client::IClientTransportFactory {
public:
    AsioTcpTransportFactory(asio::io_context &context, AsioExecutor::Strand strand) : context_(context), strand_(strand) {}

    [[nodiscard]] std::unique_ptr<uwb::client::IClientTransport> create(const uwb::client::Endpoint &endpoint,
                                                                       uwb::client::ClientConnection &owner) override;

private:
    asio::io_context &context_;
    AsioExecutor::Strand strand_;
};

// UDP discovery transport (specification §8): one socket, broadcast unicast.
class AsioUdpDiscoveryTransport final : public uwb::client::IDiscoveryTransport {
public:
    explicit AsioUdpDiscoveryTransport(asio::io_context &context, std::uint16_t port);
    ~AsioUdpDiscoveryTransport() override;

    void setHandler(std::function<void(const uwb::client::Endpoint &source, uwb::protocol::ConstBytes datagram)> handler) override;
    void send(const uwb::client::Endpoint &target, uwb::protocol::ConstBytes datagram) override;
    [[nodiscard]] bool isOpen() const override;
    [[nodiscard]] std::string lastError() const override;

    [[nodiscard]] std::uint16_t boundPort() const noexcept { return boundPort_; }

private:
    struct State : public std::enable_shared_from_this<State> {
        asio::ip::udp::socket socket;
        asio::ip::udp::endpoint sender;
        std::array<std::uint8_t, 2048> buffer{};
        bool active = false;
        std::string lastError;

        explicit State(asio::io_context &context, std::uint16_t port);
        void start();
        void arm();
    };

    void armReceive();

    asio::io_context &context_;
    std::uint16_t port_;
    std::uint16_t boundPort_ = 0;
    std::shared_ptr<State> state_;
    std::function<void(const uwb::client::Endpoint &, uwb::protocol::ConstBytes)> handler_;
    std::string lastError_;
};

// Convenience wiring used by the CLI and the integration tests: one io_context,
// one executor, one tick timer, one controller.
class ClientRuntime final {
public:
    explicit ClientRuntime(uwb::client::ControllerConfig config);

    [[nodiscard]] uwb::client::ControllerConfig &config() noexcept { return controllerConfig_; }
    ~ClientRuntime();

    ClientRuntime(const ClientRuntime &) = delete;
    ClientRuntime &operator=(const ClientRuntime &) = delete;

    void start();
    void stop();

    // Blocks the calling thread in the single I/O loop.
    void run();
    void poll();
    void stopLoop();

    [[nodiscard]] uwb::client::ApplicationController &controller() noexcept { return *controller_; }
    [[nodiscard]] AsioExecutor &executor() noexcept { return executor_; }
    [[nodiscard]] asio::io_context &context() noexcept { return context_; }
    [[nodiscard]] std::uint64_t ticks() const noexcept { return tickTimer_ ? tickTimer_->ticks() : 0; }

private:
    asio::io_context context_;
    AsioExecutor executor_;
    AsioTcpTransportFactory factory_;
    uwb::client::ControllerConfig controllerConfig_;
    std::unique_ptr<AsioUdpDiscoveryTransport> discovery_;
    std::unique_ptr<TickTimer> tickTimer_;
    uwb::client::SystemClock clock_;
    std::unique_ptr<uwb::client::ApplicationController> controller_;
    bool running_ = false;
};

} // namespace uwb::client_net
