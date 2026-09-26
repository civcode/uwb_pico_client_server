#pragma once

#include <cstdint>
#include <functional>
#include "uwb/protocol/bytes.hpp"
#include <memory>
#include <string>
#include <vector>

#include "uwb/client/client_clock.hpp"
#include "uwb/client/client_transport.hpp"
#include "uwb/client/client_error.hpp"
#include "uwb/client/client_types.hpp"
#include "uwb/protocol/payloads.hpp"

namespace uwb::client {

// UDP discovery transport (specification §55). Implemented by the Asio net
// layer; the core never touches sockets.
class IDiscoveryTransport {
public:
    virtual ~IDiscoveryTransport() = default;

    virtual void setHandler(std::function<void(const Endpoint &source, uwb::protocol::ConstBytes datagram)> handler) = 0;
    virtual void send(const Endpoint &target, uwb::protocol::ConstBytes datagram) = 0;
    [[nodiscard]] virtual bool isOpen() const = 0;
    [[nodiscard]] virtual std::string lastError() const { return {}; }
};

struct DiscoveryConfig {
    std::uint16_t port = uwb::protocol::kDefaultProtocolPort;

    // Targets to probe. Empty means "use the default broadcast addresses".
    std::vector<Endpoint> targets;
    bool useBroadcastTargets = true;

    std::uint32_t requestIntervalMs = 5000;
    std::uint32_t responseTimeoutMs = 2000;

    // Capability hint copied into the request; devices answer with their own
    // known mask, so zero is a perfectly valid probe.
    uwb::protocol::CapabilityMask requestedMask = 0;
};

// Periodic Device Identification Request sender and decoder (specification §8).
class DiscoveryClient {
public:
    using DeviceSeenCallback = std::function<void(const Endpoint &source, const uwb::protocol::DeviceIdResponse &identity)>;
    using ErrorCallback = std::function<void(const ClientError &error)>;

    DiscoveryClient(const DiscoveryConfig &config, IClock &clock, ITaskExecutor &executor,
                    std::unique_ptr<IDiscoveryTransport> transport);

    void start();
    void stop();
    void tick(std::uint64_t nowUs);
    void setDeviceSeenCallback(DeviceSeenCallback callback);
    void setErrorCallback(ErrorCallback callback);

    void probeNow();

    // One-shot probe of a single endpoint ("connect by address", specification §58).
    void probeTarget(const Endpoint &endpoint);
    [[nodiscard]] const std::vector<Endpoint> &targets() const noexcept { return targets_; }
    [[nodiscard]] std::uint64_t requestsSent() const noexcept { return requestsSent_; }
    [[nodiscard]] std::uint64_t responsesReceived() const noexcept { return responsesReceived_; }
    [[nodiscard]] std::uint64_t malformedDatagrams() const noexcept { return malformedDatagrams_; }
    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] const DiscoveryConfig &config() const noexcept { return config_; }

private:
    void sendRequests(std::uint64_t nowUs);
    [[nodiscard]] const uwb::protocol::ByteBuffer *requestFrame();

    DiscoveryConfig config_;
    IClock &clock_;
    ITaskExecutor &executor_;
    std::unique_ptr<IDiscoveryTransport> transport_;
    std::vector<Endpoint> targets_;
    uwb::protocol::ByteBuffer requestFrame_;

    DeviceSeenCallback deviceSeenCallback_;
    ErrorCallback errorCallback_;

    bool active_ = false;
    std::uint64_t lastRequestUs_ = 0;
    std::uint64_t requestsSent_ = 0;
    std::uint64_t responsesReceived_ = 0;
    std::uint64_t malformedDatagrams_ = 0;
};

} // namespace uwb::client
