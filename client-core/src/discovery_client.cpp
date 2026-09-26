#include "uwb/client/discovery_client.hpp"

#include <utility>

#include "uwb/protocol/constants.hpp"
#include "uwb/protocol/frame_parser.hpp"
#include "uwb/protocol/generic_header.hpp"

namespace uwb::client {

namespace {

// Broadcast targets used when the application did not configure explicit ones
// (specification §8, implementation plan §24.1).
[[nodiscard]] std::vector<Endpoint> defaultTargets(std::uint16_t port) {
    return {
        Endpoint{"255.255.255.255", port},
        Endpoint{"127.255.255.255", port},
    };
}

} // namespace

DiscoveryClient::DiscoveryClient(const DiscoveryConfig &config, IClock &clock, ITaskExecutor &executor,
                                 std::unique_ptr<IDiscoveryTransport> transport)
    : config_(config), clock_(clock), executor_(executor), transport_(std::move(transport)) {
    targets_ = config_.targets;
    if (targets_.empty() && config_.useBroadcastTargets) {
        targets_ = defaultTargets(config_.port);
    }
    if (targets_.empty()) {
        targets_.push_back(Endpoint{"127.0.0.1", config_.port});
    }
}

void DiscoveryClient::setDeviceSeenCallback(DeviceSeenCallback callback) { deviceSeenCallback_ = std::move(callback); }
void DiscoveryClient::setErrorCallback(ErrorCallback callback) { errorCallback_ = std::move(callback); }

void DiscoveryClient::start() {
    active_ = true;
    lastRequestUs_ = 0; // force an immediate probe

    if (transport_) {
        transport_->setHandler([this](const Endpoint &source, uwb::protocol::ConstBytes datagram) {
            // One UDP datagram carries exactly one frame (§8).
            uwb::protocol::FrameParser parser(uwb::protocol::kMaxProtocolPayload);
            auto pushed = parser.push(datagram);
            if (!pushed.ok() || parser.bufferedFrames() == 0) {
                malformedDatagrams_++;
                if (errorCallback_) {
                    errorCallback_(makeError(ErrorDomain::Protocol, ClientErrorCode::FramingError,
                                            "malformed discovery datagram"));
                }
                return;
            }

            auto frame = parser.popFrame();
            if (!frame.ok()) {
                malformedDatagrams_++;
                return;
            }
            if (frame.value().type() != uwb::protocol::PayloadType::DeviceIdResponse) {
                malformedDatagrams_++;
                return;
            }

            auto identity = uwb::protocol::decodeDeviceIdResponse(frame.value().payloadBytes());
            if (!identity.ok()) {
                malformedDatagrams_++;
                if (errorCallback_) {
                    errorCallback_(makeError(ErrorDomain::Protocol, ClientErrorCode::DecodeError,
                                            "malformed Device Id Response"));
                }
                return;
            }

            responsesReceived_++;
            if (deviceSeenCallback_) {
                deviceSeenCallback_(source, identity.value());
            }
        });
    }

    sendRequests(clock_.nowUs());
}

void DiscoveryClient::stop() {
    active_ = false;
    if (transport_) {
        transport_->setHandler({});
    }
}

void DiscoveryClient::probeNow() { lastRequestUs_ = 0; }

void DiscoveryClient::tick(std::uint64_t nowUs) {
    if (!active_) {
        return;
    }
    const std::uint64_t intervalUs = static_cast<std::uint64_t>(config_.requestIntervalMs) * 1000ULL;
    if (lastRequestUs_ != 0 && nowUs < lastRequestUs_ + intervalUs) {
        return;
    }
    sendRequests(nowUs);
}

void DiscoveryClient::sendRequests(std::uint64_t nowUs) {
    if (!transport_ || !transport_->isOpen()) {
        return;
    }

    const uwb::protocol::ByteBuffer *frame = requestFrame();
    if (frame == nullptr) {
        return;
    }
    for (const Endpoint &target : targets_) {
        transport_->send(target, uwb::protocol::bytesOf(*frame));
        requestsSent_++;
    }
    lastRequestUs_ = nowUs;
}

// The Device Id Request body is constant, so it is built once and reused.
const uwb::protocol::ByteBuffer *DiscoveryClient::requestFrame() {
    if (requestFrame_.empty()) {
        const uwb::protocol::ByteBuffer body = uwb::protocol::encodeDeviceIdRequest(uwb::protocol::DeviceIdRequest{});
        auto encoded = uwb::protocol::encodeFrame(uwb::protocol::PayloadType::DeviceIdRequest,
                                                  uwb::protocol::bytesOf(body));
        if (!encoded.ok()) {
            return nullptr;
        }
        requestFrame_ = std::move(encoded.value());
    }
    return &requestFrame_;
}

void DiscoveryClient::probeTarget(const Endpoint &endpoint) {
    if (!transport_ || !transport_->isOpen() || endpoint.host.empty()) {
        return;
    }
    const uwb::protocol::ByteBuffer *frame = requestFrame();
    if (frame == nullptr) {
        return;
    }
    transport_->send(endpoint, uwb::protocol::bytesOf(*frame));
    requestsSent_++;
}

} // namespace uwb::client
