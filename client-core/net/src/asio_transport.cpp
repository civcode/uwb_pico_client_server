#include "uwb/client_net/asio_transport.hpp"

#include <system_error>
#include <utility>

#include <asio/bind_executor.hpp>
#include <asio/post.hpp>
#include <asio/write.hpp>
#include <asio/ip/address.hpp>

namespace uwb::client_net {

namespace {

using namespace uwb::client;

[[nodiscard]] Endpoint toClientEndpoint(const asio::ip::udp::endpoint &remote) {
    return Endpoint{remote.address().to_string(), remote.port()};
}

// Asio 1.28 removed strand::wrap(); bind_executor is the replacement.
template <typename Handler>
[[nodiscard]] auto onStrand(AsioExecutor::Strand &strand, Handler &&handler) {
    return asio::bind_executor(strand, std::forward<Handler>(handler));
}

} // namespace

// ---------------------------------------------------------------------------
// AsioExecutor
// ---------------------------------------------------------------------------

AsioExecutor::AsioExecutor(asio::io_context &context) : context_(context) {}


void AsioExecutor::post(std::function<void()> task) {
    asio::post(context_.get_executor(), std::move(task));
}

void AsioExecutor::run() {
    stopped_ = false;
    context_.run();
    stopped_ = true;
}

std::size_t AsioExecutor::poll() { return context_.poll(); }

std::size_t AsioExecutor::runFor(std::chrono::milliseconds timeout) {
    context_.restart();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::size_t executed = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const std::size_t done = context_.run_one();
        if (done == 0) {
            break;
        }
        executed += done;
    }
    return executed;
}

void AsioExecutor::stop() { context_.stop(); }

// ---------------------------------------------------------------------------
// TickTimer
// ---------------------------------------------------------------------------

TickTimer::TickTimer(asio::io_context &context, std::chrono::milliseconds interval, std::function<void()> callback)
    : timer_(context), interval_(interval), callback_(std::move(callback)) {}

void TickTimer::start() {
    active_ = true;
    arm();
}

void TickTimer::stop() {
    active_ = false;
    timer_.cancel();
}

void TickTimer::arm() {
    if (!active_) {
        return;
    }
    timer_.expires_after(interval_);
    timer_.async_wait([this](const asio::error_code &error) {
        if (error || !active_) {
            return;
        }
        ticks_++;
        if (callback_) {
            callback_();
        }
        arm();
    });
}

// ---------------------------------------------------------------------------
// AsioTcpTransport
// ---------------------------------------------------------------------------

AsioTcpTransport::State::State(asio::io_context &context, AsioExecutor::Strand owningStrand,
                               asio::ip::tcp::endpoint remoteEndpoint, ClientConnection &connection)
    : socket(context), strand(owningStrand), deadline(context), remote(remoteEndpoint), owner(connection) {}

asio::ip::tcp::endpoint remoteFrom(const Endpoint &endpoint) {
    std::error_code error;
    const auto address = asio::ip::make_address(endpoint.host, error);
    if (error) {
        return asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), endpoint.port);
    }
    return asio::ip::tcp::endpoint(address, endpoint.port);
}

AsioTcpTransport::AsioTcpTransport(asio::io_context &context, AsioExecutor::Strand strand,
                                   asio::ip::tcp::endpoint remote, ClientConnection &owner)
    : state_(std::make_shared<State>(context, strand, remote, owner)) {}

AsioTcpTransport::~AsioTcpTransport() {
    if (state_) {
        state_->closing = true;
        state_->deadline.cancel();
        asio::error_code ignored;
        state_->socket.close(ignored);
    }
}

void AsioTcpTransport::connect(std::uint32_t timeoutMs, std::function<void(TransportStatus, const std::string &)> onDone) {
    state_->connectDone = std::move(onDone);
    state_->beginConnect(timeoutMs);
}

void AsioTcpTransport::State::beginConnect(std::uint32_t timeoutMs) {
    if (closing) {
        finish(TransportStatus::Closed, "transport was closed before connect");
        return;
    }

    connecting = true;
    asio::error_code error;
    socket.open(remote.address().is_v6() ? asio::ip::tcp::v6() : asio::ip::tcp::v4(), error);
    if (error) {
        finish(TransportStatus::ConnectFailed, error.message());
        return;
    }

    if (timeoutMs != 0) {
        deadline.expires_after(std::chrono::milliseconds(timeoutMs));
        auto self = shared_from_this();
        deadline.async_wait(onStrand(strand, [self](const asio::error_code &timerError) {
            if (timerError || !self->connecting) {
                return; // connect completed first
            }
            asio::error_code ignored;
            self->socket.close(ignored);
            self->finish(TransportStatus::ConnectFailed, "connect timed out");
        }));
    }

    auto self = shared_from_this();
    socket.async_connect(remote, onStrand(strand, [self](const asio::error_code &connectError) {
        if (!self->connecting) {
            return; // deadline already fired
        }
        self->connecting = false;
        self->deadline.cancel();
        if (connectError) {
            self->finish(TransportStatus::ConnectFailed, connectError.message());
            return;
        }
        self->open = true;
        if (self->connectDone) {
            const auto callback = std::move(self->connectDone);
            callback(TransportStatus::Ok, std::string{});
        }
        self->owner.onConnected();
        self->startRead();
    }));
}

void AsioTcpTransport::State::startRead() {
    if (!open || closing) {
        return;
    }
    auto self = shared_from_this();
    socket.async_read_some(asio::buffer(readBuffer), onStrand(strand, [self](const asio::error_code &error, std::size_t count) {
        if (self->closing) {
            return;
        }
        if (error) {
            const bool clean = error == asio::error::eof;
            self->finish(clean ? TransportStatus::Closed : TransportStatus::ConnectFailed,
                         clean ? "connection closed by device" : error.message());
            return;
        }
        self->owner.onBytes(uwb::protocol::ConstBytes{self->readBuffer.data(), count});
        if (!self->closing) {
            self->startRead();
        }
    }));
}

void AsioTcpTransport::write(const uwb::protocol::ConstBytes frame) {
    state_->pendingWrite.assign(frame.begin(), frame.end());
    state_->pump();
}

void AsioTcpTransport::State::pump() {
    if (!open || closing || pendingWrite.empty()) {
        return;
    }

    auto self = shared_from_this();
    asio::async_write(socket, asio::buffer(pendingWrite), onStrand(strand, [self](const asio::error_code &error, std::size_t) {
        if (self->closing) {
            self->pendingWrite.clear();
            return;
        }
        if (error) {
            self->pendingWrite.clear();
            self->finish(TransportStatus::WriteFailed, error.message());
            return;
        }
        self->pendingWrite.clear();
    }));
}

void AsioTcpTransport::State::finish(TransportStatus status, const std::string &message) {
    if (closing) {
        return;
    }
    closing = true;
    open = false;
    connecting = false;
    lastError = message;

    asio::error_code ignored;
    socket.close(ignored);
    deadline.cancel();

    if (connectDone) {
        const auto callback = std::move(connectDone);
        callback(status == TransportStatus::Ok ? TransportStatus::Ok : status, message);
    }
    owner.onClosed(status, message);
}

void AsioTcpTransport::close() {
    state_->closing = true;
    state_->open = false;
    state_->connecting = false;
    state_->pendingWrite.clear();
    state_->deadline.cancel();
    asio::error_code ignored;
    state_->socket.close(ignored);
}

bool AsioTcpTransport::isOpen() const noexcept { return state_->open; }

std::unique_ptr<IClientTransport> AsioTcpTransportFactory::create(const Endpoint &endpoint, ClientConnection &owner) {
    if (endpoint.host.empty()) {
        return {};
    }
    return std::make_unique<AsioTcpTransport>(context_, strand_, remoteFrom(endpoint), owner);
}

// ---------------------------------------------------------------------------
// AsioUdpDiscoveryTransport
// ---------------------------------------------------------------------------

// The client socket binds an ephemeral port: devices answer the UDP source
// port of the request, so binding the protocol port itself would collide with
// the device (§8).
AsioUdpDiscoveryTransport::State::State(asio::io_context &context, std::uint16_t)
    : socket(context, asio::ip::udp::endpoint(asio::ip::udp::v4(), 0)) {
    asio::error_code error;
    socket.set_option(asio::socket_base::broadcast(true), error);
    lastError = error.message();
    socket.set_option(asio::socket_base::reuse_address(true), error);
    if (error) {
        lastError = error.message();
    }
}

AsioUdpDiscoveryTransport::AsioUdpDiscoveryTransport(asio::io_context &context, std::uint16_t port)
    : context_(context), port_(port) {}

AsioUdpDiscoveryTransport::~AsioUdpDiscoveryTransport() {
    if (state_) {
        state_->active = false;
        asio::error_code ignored;
        state_->socket.close(ignored);
    }
}

void AsioUdpDiscoveryTransport::setHandler(
    std::function<void(const Endpoint &source, uwb::protocol::ConstBytes datagram)> handler) {
    handler_ = std::move(handler);

    if (!handler_ && state_) {
        state_->active = false;
        asio::error_code ignored;
        state_->socket.close(ignored);
        state_.reset();
        return;
    }

    if (handler_ && !state_) {
        try {
            state_ = std::make_shared<State>(context_, port_);
            boundPort_ = static_cast<std::uint16_t>(state_->socket.local_endpoint().port());
        } catch (const std::exception &error) {
            lastError_ = error.what();
            state_.reset();
            return;
        }
        state_->active = true;
        armReceive();
    }
}

void AsioUdpDiscoveryTransport::armReceive() {
    if (!state_ || !state_->active) {
        return;
    }
    auto state = state_;
    state->socket.async_receive_from(asio::buffer(state->buffer), state->sender,
                                    [this, state](const asio::error_code &error, std::size_t read) {
                                        if (!state->active) {
                                            return;
                                        }
                                        if (error) {
                                            if (error != asio::error::operation_aborted) {
                                                lastError_ = error.message();
                                            }
                                            return;
                                        }
                                        if (handler_ && read > 0) {
                                            handler_(toClientEndpoint(state->sender),
                                                     uwb::protocol::ConstBytes{state->buffer.data(), read});
                                        }
                                        armReceive();
                                    });
}

void AsioUdpDiscoveryTransport::send(const Endpoint &target, uwb::protocol::ConstBytes datagram) {
    if (!state_ || !state_->active) {
        lastError_ = "discovery socket is not open";
        return;
    }

    std::error_code error;
    const auto address = asio::ip::make_address(target.host, error);
    if (error) {
        lastError_ = "invalid discovery target: " + target.host;
        return;
    }

    const asio::ip::udp::endpoint remote(address, target.port);
    asio::error_code sent;
    state_->socket.send_to(asio::buffer(datagram.data(), datagram.size()), remote, 0, sent);
    if (sent) {
        lastError_ = sent.message();
    }
}

bool AsioUdpDiscoveryTransport::isOpen() const { return state_ != nullptr && state_->active; }

std::string AsioUdpDiscoveryTransport::lastError() const { return lastError_.empty() && state_ ? state_->lastError : lastError_; }

// ---------------------------------------------------------------------------
// ClientRuntime
// ---------------------------------------------------------------------------

ClientRuntime::ClientRuntime(uwb::client::ControllerConfig config)
    : context_(1), executor_(context_), factory_(context_, executor_.strand()), controllerConfig_(std::move(config)),
      clock_() {}

ClientRuntime::~ClientRuntime() { stop(); }

void ClientRuntime::start() {
    if (running_) {
        return;
    }
    running_ = true;

    discovery_ = std::make_unique<AsioUdpDiscoveryTransport>(context_, controllerConfig_.discovery.port);
    controller_ = std::make_unique<uwb::client::ApplicationController>(controllerConfig_, clock_, executor_, factory_,
                                                                      std::move(discovery_));
    controller_->start();

    tickTimer_ = std::make_unique<TickTimer>(context_, std::chrono::milliseconds(50), [this] {
        static_cast<void>(clock_.nowUs());
        controller_->tick(clock_.nowUs());
    });
    tickTimer_->start();
}

void ClientRuntime::stop() {
    if (!running_) {
        return;
    }
    running_ = false;
    if (tickTimer_) {
        tickTimer_->stop();
    }
    if (controller_) {
        controller_->stop();
    }
}

void ClientRuntime::run() { executor_.run(); }
void ClientRuntime::poll() { static_cast<void>(executor_.poll()); }
void ClientRuntime::stopLoop() { executor_.stop(); }

} // namespace uwb::client_net
