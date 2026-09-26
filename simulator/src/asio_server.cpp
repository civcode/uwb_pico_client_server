#include "uwb/simulator/asio_server.hpp"

#include <cstdio>
#include <utility>
#include <vector>

#include "uwb/protocol/bytes.hpp"

namespace uwb::simulator {
namespace {

constexpr std::uint16_t kDiscoveryUdpPortFallback = 13401;

asio::ip::address parseAddress(const std::string &text, std::string &error) {
    asio::error_code ec;
    const asio::ip::address address = asio::ip::make_address(text, ec);
    if (ec) {
        error = "invalid bind address: " + text;
        return asio::ip::address_v4::any();
    }
    return address;
}

} // namespace

SimulatorServer::SimulatorServer(asio::io_context &io, SimulatorRuntime &runtime, const SimulatorOptions &options)
    : io_(io), runtime_(runtime), options_(options), udpSocket_(io), acceptor_(io), tickTimer_(io) {}

SimulatorServer::~SimulatorServer() {
    stop();
}

bool SimulatorServer::start(std::string &error) {
    std::string addressError;
    const asio::ip::address bindAddress = parseAddress(options_.bindAddress, addressError);
    if (!addressError.empty()) {
        error = addressError;
        return false;
    }

    const bool ipv6 = bindAddress.is_v6();

    asio::error_code ec;
    udpSocket_.open(ipv6 ? asio::ip::udp::v6() : asio::ip::udp::v4(), ec);
    if (!ec) {
        udpSocket_.set_option(asio::socket_base::reuse_address(true), ec);
        udpSocket_.bind(asio::ip::udp::endpoint(bindAddress, options_.udpPort), ec);
    }
    if (ec) {
        error = "UDP bind failed on port " + std::to_string(options_.udpPort) + ": " + ec.message();
        return false;
    }

    acceptor_.open(ipv6 ? asio::ip::tcp::v6() : asio::ip::tcp::v4(), ec);
    if (!ec) {
        acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
        acceptor_.bind(asio::ip::tcp::endpoint(bindAddress, options_.tcpPort), ec);
    }
    if (!ec) {
        acceptor_.listen(8, ec);
    }
    if (ec) {
        error = "TCP bind failed on port " + std::to_string(options_.tcpPort) + ": " + ec.message();
        return false;
    }

    if (options_.udpPort == 0U) {
        options_.udpPort = udpSocket_.local_endpoint().port();
    }
    if (options_.tcpPort == 0U) {
        options_.tcpPort = acceptor_.local_endpoint().port();
        runtime_.core().configureIdentity(options_.boardUniqueId, options_.deviceName, options_.logicalAddress,
                                          options_.tcpPort);
        runtime_.core().clearBackendReinitRequest();
    }
    if (options_.udpPort == 0U) {
        options_.udpPort = kDiscoveryUdpPortFallback;
    }

    startAccept();
    startDiscoveryRead();
    scheduleTick();
    return true;
}

void SimulatorServer::run() {
    if (stopping_.load(std::memory_order_acquire)) {
        return;
    }
    running_ = true;
    asio::error_code ec;
    io_.run(ec);
    running_ = false;
}

void SimulatorServer::poll() { io_.poll(); }

void SimulatorServer::requestStop() noexcept {
    stopping_.store(true, std::memory_order_release);
    tickTimer_.cancel();
    asio::error_code ec;
    acceptor_.close(ec);
    udpSocket_.close(ec);
    io_.stop(); // pending completions are discarded; run() returns
}

void SimulatorServer::stop() noexcept {
    requestStop();
    teardownConnections();
}

void SimulatorServer::teardownConnections() noexcept {
    asio::error_code ec;
    for (auto &entry : connections_) {
        entry.second->closing = true;
        entry.second->open = false;
        entry.second->socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        entry.second->socket.close(ec);
    }
    connections_.clear();
}

std::uint16_t SimulatorServer::udpPort() const noexcept {
    if (udpSocket_.is_open()) {
        return udpSocket_.local_endpoint().port();
    }
    return options_.udpPort;
}

std::uint16_t SimulatorServer::tcpPort() const noexcept {
    if (acceptor_.is_open()) {
        return acceptor_.local_endpoint().port();
    }
    return options_.tcpPort;
}

// ---------------------------------------------------------------------------
// TCP accept / read / write
// ---------------------------------------------------------------------------
void SimulatorServer::startAccept() {
    acceptor_.async_accept([this](std::error_code ec, asio::ip::tcp::socket socket) {
        onAccepted(ec, std::move(socket));
    });
}

void SimulatorServer::onAccepted(std::error_code ec, asio::ip::tcp::socket socket) {
    if (stopping_.load(std::memory_order_acquire)) {
        return; // shutdown wins: the socket is dropped without registration
    }
    if (ec) {
        if (ec != asio::error::operation_aborted && acceptor_.is_open()) {
            startAccept();
        }
        return;
    }

    const auto id = nextConnectionId_++;
    ConnectionPtr connection = std::make_shared<TcpConnection>(*this, io_, id);
    connection->socket = std::move(socket);
    connection->open = true;
    connections_.emplace(id, connection);

    asio::error_code ignored;
    const std::uint16_t remotePort = connection->socket.remote_endpoint(ignored).port();
    runtime_.onConnect(id, *connection, uwb::server::PeerInfo{remotePort});
    startRead(connection);

    if (options_.verbose) {
        std::printf("[sim] connection %u opened (port %u)\n", id, remotePort);
    }
    if (acceptor_.is_open()) {
        startAccept();
    }
}

void SimulatorServer::startRead(const ConnectionPtr &connection) {
    connection->socket.async_read_some(asio::buffer(connection->readBuffer),
                                       [connection, this](std::error_code ec, std::size_t bytesRead) {
                                           onRead(connection, ec, bytesRead);
                                       });
}

void SimulatorServer::onRead(const ConnectionPtr &connection, std::error_code ec, std::size_t bytesRead) {
    if (connection->closing || stopping_.load(std::memory_order_acquire)) {
        return; // the connection was already closed; do not touch a dead socket
    }
    if (ec) {
        closeConnection(connection->id);
        return;
    }
    const uwb::protocol::ConstBytes bytes(connection->readBuffer.data(), bytesRead);
    runtime_.onBytes(connection->id, bytes);
    if (!runtime_.wantsConnectionOpen(connection->id)) {
        closeConnection(connection->id);
        return;
    }
    startRead(connection);
}

bool SimulatorServer::TcpConnection::writeFrame(uwb::protocol::ConstBytes frame) {
    if (!open) {
        return false;
    }
    if (out.size() >= kMaxQueuedFrames) {
        return false; // back-pressure: ServerCore keeps the frame queued (§38)
    }
    out.emplace_back(frame.begin(), frame.end());
    owner.startNextWrite(shared_from_this());
    return true;
}

void SimulatorServer::startNextWrite(const ConnectionPtr &connection) {
    if (connection->writing || connection->out.empty() || !connection->open || connection->closing) {
        return;
    }
    connection->inFlight = std::move(connection->out.front());
    connection->out.pop_front();
    connection->writing = true;
    asio::async_write(connection->socket, asio::buffer(connection->inFlight),
                      [connection, this](std::error_code ec, std::size_t) { onWritten(connection, ec); });
}

void SimulatorServer::onWritten(const ConnectionPtr &connection, std::error_code ec) {
    connection->writing = false;
    connection->inFlight.clear();
    if (connection->closing || stopping_.load(std::memory_order_acquire)) {
        return;
    }
    if (ec) {
        closeConnection(connection->id);
        return;
    }
    startNextWrite(connection);
}

void SimulatorServer::closeConnection(uwb::server::ConnectionId id) noexcept {
    if (stopping_.load(std::memory_order_acquire)) {
        return; // teardownConnections() owns the bookkeeping during shutdown
    }
    const auto it = connections_.find(id);
    if (it == connections_.end()) {
        return;
    }
    ConnectionPtr connection = it->second;
    connections_.erase(it); // any pending completion keeps the object alive

    if (options_.verbose) {
        std::printf("[sim] connection %u closed\n", id);
    }
    asio::error_code ec;
    connection->closing = true;
    connection->open = false;
    connection->writing = false;
    connection->out.clear();
    connection->inFlight.clear();
    connection->socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    connection->socket.close(ec);
    runtime_.onDisconnect(id); // safe to call twice; the core ignores unknown ids
}

// ---------------------------------------------------------------------------
// UDP discovery (§8, §17)
// ---------------------------------------------------------------------------
void SimulatorServer::startDiscoveryRead() {
    udpSocket_.async_receive_from(asio::buffer(udpBuffer_), senderEndpoint_,
                                 [this](std::error_code ec, std::size_t size) {
                                     onDiscoveryDatagram(ec, size, senderEndpoint_);
                                 });
}

void SimulatorServer::onDiscoveryDatagram(std::error_code ec, std::size_t size, asio::ip::udp::endpoint sender) {
    if (stopping_.load(std::memory_order_acquire)) {
        return;
    }
    if (!ec && size != 0U) {
        const uwb::protocol::ConstBytes datagram(udpBuffer_.data(), size);
        if (const auto response = runtime_.discoveryResponse(datagram, uwb::protocol::Uuid{}); response.has_value()) {
            asio::error_code ignored;
            udpSocket_.send_to(asio::buffer(*response), sender, static_cast<asio::socket_base::message_flags>(0),
                               ignored);
            if (options_.verbose && !ignored) {
                std::printf("[sim] discovery reply to %s:%u\n", sender.address().to_string().c_str(), sender.port());
            }
        }
    }
    if (udpSocket_.is_open()) {
        startDiscoveryRead();
    }
}

// ---------------------------------------------------------------------------
// Periodic protocol tick (§44 poll loop equivalent)
// ---------------------------------------------------------------------------
void SimulatorServer::scheduleTick() {
    tickTimer_.expires_after(std::chrono::milliseconds(kTickIntervalMs));
    tickTimer_.async_wait([this](std::error_code ec) { onTick(ec); });
}

void SimulatorServer::onTick(std::error_code ec) {
    if (ec || stopping_.load(std::memory_order_acquire)) {
        return;
    }
    runtime_.tick();
    closeIdleConnections();
    scheduleTick();
}

void SimulatorServer::closeIdleConnections() {
    std::vector<uwb::server::ConnectionId> toClose;
    for (const auto &entry : connections_) {
        if (!runtime_.wantsConnectionOpen(entry.first)) {
            toClose.push_back(entry.first);
        }
    }
    for (const auto id : toClose) {
        closeConnection(id);
    }
}

} // namespace uwb::simulator
