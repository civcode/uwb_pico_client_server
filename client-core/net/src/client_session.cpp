#include "uwb/client_net/client_session.hpp"

#include <chrono>
#include <thread>
#include <utility>

namespace uwb::client_net {

using namespace std::chrono_literals;

ClientSession::ClientSession(uwb::client::ControllerConfig config) : runtime_(std::move(config)) {}

ClientSession::~ClientSession() { shutdown(); }

void ClientSession::start() {
    if (running_.load()) {
        return;
    }
    runtime_.start();
    ioThread_ = std::thread([this] { runtime_.run(); });
    ioThreadId_.store(ioThread_.get_id());
    running_.store(true);
}

void ClientSession::shutdown() {
    if (!running_.load()) {
        return;
    }

    // Closing runs on the owner thread; the worker keeps spinning afterwards so
    // the close frames actually leave the socket.
    invoke([this] { runtime_.controller().disconnectAll(); });
    std::this_thread::sleep_for(150ms);

    runtime_.stopLoop();
    if (ioThread_.joinable()) {
        ioThread_.join();
    }
    running_.store(false);
    runtime_.stop();
}

void ClientSession::discoverNow() {
    invoke([this] { runtime_.controller().discoverNow(); });
}

std::vector<uwb::client::DeviceSummary> ClientSession::listDevices() {
    return invoke([this] { return runtime_.controller().listDevices(); });
}

std::optional<uwb::client::DeviceSummary> ClientSession::deviceSummary(const uwb::domain::DeviceUuid &uuid) {
    return invoke([this, uuid] { return runtime_.controller().deviceSummary(uuid); });
}

std::optional<uwb::domain::DeviceUuid> ClientSession::resolveDevice(std::string_view nameOrUuid) {
    const std::string text(nameOrUuid);
    return invoke([this, text] { return runtime_.controller().resolveDevice(text); });
}

uwb::client::DiagnosticsSnapshot ClientSession::diagnostics() {
    return invoke([this] { return runtime_.controller().diagnostics(); });
}

uwb::client::ClientResult<uwb::domain::DeviceUuid> ClientSession::connectDevice(const uwb::domain::DeviceUuid &uuid) {
    return invoke([this, uuid] { return runtime_.controller().connectDevice(uuid); });
}

uwb::client::ClientResult<std::string> ClientSession::connectEndpoint(const uwb::client::Endpoint &endpoint,
                                                                     std::string_view label) {
    const std::string text(label);
    return invoke([this, endpoint, text] { return runtime_.controller().connectEndpoint(endpoint, text); });
}

void ClientSession::disconnectDevice(const uwb::domain::DeviceUuid &uuid) {
    invoke([this, uuid] { runtime_.controller().disconnectDevice(uuid); });
}

void ClientSession::disconnectAll() {
    invoke([this] { runtime_.controller().disconnectAll(); });
}

} // namespace uwb::client_net
