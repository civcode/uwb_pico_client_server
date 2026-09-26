#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "uwb/client/application_controller.hpp"
#include "uwb/client/client_types.hpp"
#include "uwb/client/notification_queue.hpp"
#include "uwb/client_net/asio_transport.hpp"

namespace uwb::client_net {

// Thread-safe façade over ApplicationController for applications that keep their
// own main thread (CLI, GUI, test harness).
//
// Threading contract (specification §55, plan §25): exactly one I/O thread owns
// every TCP connection, the device registry, and the discovery socket.  No
// controller method may therefore run on the application thread.
//
//   post(task)    - runs task on the I/O thread, does not wait
//   invoke(task)  - runs task on the I/O thread and waits for its result
//
// NotificationQueue is the one object the application thread may touch directly:
// it is internally synchronised and is the boundary between the I/O thread and
// slow consumers (specification §56, §60).
class ClientSession final {
public:
    explicit ClientSession(uwb::client::ControllerConfig config);
    ~ClientSession();

    ClientSession(const ClientSession &) = delete;
    ClientSession &operator=(const ClientSession &) = delete;

    void start();

    // Closes every connection, gives the I/O thread time to flush the closing
    // frames, then stops the loop and joins the worker.
    void shutdown();

    [[nodiscard]] bool alive() const noexcept { return running_.load(); }

    template <typename Handler>
    void post(Handler &&handler) {
        if (!running_.load()) {
            return;
        }
        runtime_.executor().post(std::forward<Handler>(handler));
    }

    template <typename Handler>
    [[nodiscard]] decltype(auto) invoke(Handler &&handler) {
        using Value = std::invoke_result_t<Handler>;
        if (!running_.load() || std::this_thread::get_id() == ioThreadId_.load()) {
            // Already on the owner thread (or the loop is gone): run in place.
            return std::forward<Handler>(handler)();
        }

        auto box = std::make_shared<std::promise<Value>>();
        auto result = box->get_future();
        runtime_.executor().post([handler = std::forward<Handler>(handler), box]() mutable {
            if constexpr (std::is_void_v<Value>) {
                handler();
                box->set_value();
            } else {
                box->set_value(handler());
            }
        });
        return result.get();
    }

    // --- discovery and registry (marshalled onto the I/O thread) ------------
    void discoverNow();
    [[nodiscard]] std::vector<uwb::client::DeviceSummary> listDevices();
    [[nodiscard]] std::optional<uwb::client::DeviceSummary> deviceSummary(const uwb::domain::DeviceUuid &uuid);
    [[nodiscard]] std::optional<uwb::domain::DeviceUuid> resolveDevice(std::string_view nameOrUuid);
    [[nodiscard]] uwb::client::DiagnosticsSnapshot diagnostics();

    // --- connections ---------------------------------------------------------
    [[nodiscard]] uwb::client::ClientResult<uwb::domain::DeviceUuid> connectDevice(const uwb::domain::DeviceUuid &uuid);
    [[nodiscard]] uwb::client::ClientResult<std::string> connectEndpoint(const uwb::client::Endpoint &endpoint,
                                                                        std::string_view label);
    void disconnectDevice(const uwb::domain::DeviceUuid &uuid);
    void disconnectAll();

    // Notification queue: safe to read from the application thread (§56).
    [[nodiscard]] uwb::client::NotificationQueue &notifications() { return runtime_.controller().notifications(); }
    [[nodiscard]] uwb::client::ApplicationController &controller() noexcept { return runtime_.controller(); }
    [[nodiscard]] uwb::client::ControllerConfig &config() noexcept { return runtime_.config(); }

private:
    ClientRuntime runtime_;
    std::thread ioThread_;
    std::atomic<bool> running_{false};
    std::atomic<std::thread::id> ioThreadId_{};
};

} // namespace uwb::client_net
