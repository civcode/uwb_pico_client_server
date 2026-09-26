#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "uwb/client/client_types.hpp"
#include "uwb/protocol/bytes.hpp"

namespace uwb::client {

class ClientConnection;

// Transport outcome reported by the adapter to the connection object.
enum class TransportStatus : std::uint8_t {
    Ok = 0,
    ConnectFailed = 1,
    Closed = 2,
    WriteFailed = 3,
    ProtocolViolation = 4,
};

// The client core never owns a socket: it schedules work onto the single
// network I/O thread through this executor (specification §55).
class ITaskExecutor {
public:
    virtual ~ITaskExecutor() = default;
    virtual void post(std::function<void()> task) = 0;
};

// Abstract client transport. Every method is invoked on the network I/O thread;
// the adapter calls back into the owning ClientConnection on the same thread.
class IClientTransport {
public:
    virtual ~IClientTransport() = default;

    virtual void connect(std::uint32_t timeoutMs,
                         std::function<void(TransportStatus status, const std::string &message)> onDone) = 0;
    virtual void write(uwb::protocol::ConstBytes frame) = 0; // queued, never blocks
    virtual void close() = 0;
    [[nodiscard]] virtual bool isOpen() const noexcept = 0;
};

// Creates the concrete transport for one connection (Asio adapter in the
// networking layer, scripted transport in unit tests).
class IClientTransportFactory {
public:
    virtual ~IClientTransportFactory() = default;
    // The adapter must deliver onConnected / onBytes / onClosed callbacks to
    // `owner` on the same thread that owns the I/O loop.
    [[nodiscard]] virtual std::unique_ptr<IClientTransport> create(const Endpoint &endpoint,
                                                                   ClientConnection &owner) = 0;
};

} // namespace uwb::client
