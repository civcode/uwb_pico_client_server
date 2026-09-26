// Phase 4 integration tests for ClientSession (implementation plan §25, §27.8).
//
// ClientSession is the thread-safe façade the CLI and GUI use: the application
// thread stays separate from the single I/O thread that owns every connection
// (specification §55).  These tests run the real Asio adapters against a real
// simulator and only ever touch the controller through post()/invoke().

#include "test_transport.hpp"

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <uwb/client_net/client_session.hpp>

#include "uwb/client/client_types.hpp"
#include "uwb/protocol/bytes.hpp"
#include "uwb/protocol/did_records.hpp"
#include "uwb/protocol/dids.hpp"
#include "uwb/protocol/events.hpp"

using namespace std::chrono_literals;

namespace domain = uwb::domain;
namespace protocol = uwb::protocol;
using uwb::client::ClientResult;
using uwb::client::ClientErrorCode;
using uwb::client::ConnectionState;
using uwb::client::ControllerConfig;
using uwb::client::DeviceSummary;
using uwb::client::Endpoint;
using uwb::client::NotificationKind;
using uwb::client::ServiceResponse;
using uwb::client_net::ClientSession;
using uwb::simulator::SimulatorOptions;
using uwb::test::SimulatorFixture;
using uwb::test::fastOptions;

namespace {

std::unique_ptr<SimulatorFixture> makeSim(const std::string &name, std::uint8_t id) {
    SimulatorOptions options = fastOptions();
    options.boardUniqueId = std::array<std::uint8_t, 8>{id, 7, 0, 0, 0, 0, 0, id};
    options.deviceName = name;
    options.logicalAddress = static_cast<std::uint16_t>(0x1200U + id);
    options.udpPort = 0;
    options.tcpPort = 0;
    options.measurementPeriodMs = 20;
    return std::make_unique<SimulatorFixture>(std::move(options));
}

ControllerConfig sessionConfig(std::uint16_t udpPort) {
    ControllerConfig config;
    config.discovery.targets = {Endpoint{"127.0.0.1", udpPort}};
    config.discovery.useBroadcastTargets = false;
    config.discovery.requestIntervalMs = 100;
    config.options.timeouts.connectTimeoutMs = 2000;
    config.options.timeouts.requestTimeoutMs = 1000;
    config.options.timeouts.aliveCheckIdleMs = 5000;
    config.options.reconnectJitter = [](std::uint32_t) { return 0U; };
    return config;
}

domain::DeviceUuid simUuid(const SimulatorOptions &options) {
    auto uuid = protocol::deviceUuidFromBoardId(protocol::ConstBytes{options.boardUniqueId.data(), options.boardUniqueId.size()});
    REQUIRE(uuid.ok());
    return uuid.value();
}

// A latch the I/O thread fills and the application thread waits on.
class Latch {
public:
    void complete(ClientResult<std::string> result) {
        std::lock_guard<std::mutex> lock(mutex_);
        result_ = std::move(result);
    }
    [[nodiscard]] bool ready() {
        std::lock_guard<std::mutex> lock(mutex_);
        return result_.has_value();
    }
    [[nodiscard]] ClientResult<std::string> take() {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::move(*result_);
    }

private:
    std::mutex mutex_;
    std::optional<ClientResult<std::string>> result_;
};

class ReadLatch {
public:
    void complete(ClientResult<ServiceResponse> result) {
        std::lock_guard<std::mutex> lock(mutex_);
        result_ = std::move(result);
    }
    [[nodiscard]] bool ready() {
        std::lock_guard<std::mutex> lock(mutex_);
        return result_.has_value();
    }
    [[nodiscard]] std::optional<protocol::ByteBuffer> record() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!result_.has_value() || !result_->ok()) {
            return std::nullopt;
        }
        auto decoded = protocol::decodeReadDidResponse(result_->value().pduBytes());
        if (!decoded.ok()) {
            return std::nullopt;
        }
        return decoded.value().data;
    }
    // Non-zero client error code when the read failed, 0 when it succeeded.
    [[nodiscard]] int failureCode() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!result_.has_value() || result_->ok()) {
            return 0;
        }
        return result_->error().code;
    }

private:
    std::mutex mutex_;
    std::optional<ClientResult<ServiceResponse>> result_;
};

bool waitUntil(const std::function<bool()> &predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    return predicate();
}

// Waits for a marshalled read to land on the application thread.
[[nodiscard]] std::optional<protocol::ByteBuffer> awaitRead(ReadLatch &latch) {
    if (!waitUntil([&] { return latch.ready(); }, 3000ms)) {
        return std::nullopt;
    }
    return latch.record();
}

} // namespace

TEST_CASE("ClientSession runs discovery, connect, and reads off the I/O thread", "[integration][client][session]") {
    auto sim = makeSim("marshalled", 1);
    ClientSession session(sessionConfig(sim->udpPort()));
    session.start();

    CHECK(session.alive());
    CHECK(session.diagnostics().discoveryRequestsSent >= 1U);

    REQUIRE(waitUntil([&] { return session.listDevices().size() == 1U; }, 3000ms));
    const auto uuid = simUuid(sim->options);

    auto connected = session.connectDevice(uuid);
    REQUIRE(connected.ok());
    CHECK(connected.value() == uuid);

    REQUIRE(waitUntil([&] {
        auto summary = session.deviceSummary(uuid);
        return summary.has_value() && summary->state == ConnectionState::Ready;
    },
                      3000ms));

    ReadLatch nameRead;
    session.post([&session, uuid, &nameRead] {
        session.controller().readDid(uuid, protocol::Did::DeviceName,
                                     [&nameRead](ClientResult<ServiceResponse> result) {
                                         nameRead.complete(std::move(result));
                                     });
    });
    auto name = awaitRead(nameRead);
    REQUIRE(name.has_value());
    auto text = protocol::decodeTextRecord8(protocol::bytesOf(*name), protocol::kMaxDeviceNameLength);
    REQUIRE(text.ok());
    CHECK(std::string(text.value().begin(), text.value().end()) == "marshalled");

    // invoke() returns the controller value to the calling thread.
    const auto summary = session.invoke([&] { return session.controller().deviceSummary(uuid); });
    REQUIRE(summary.has_value());
    CHECK(summary->role == protocol::ConnectionRole::Control);
    CHECK(summary->endpoint.port == sim->tcpPort());

    const auto stats = session.diagnostics();
    CHECK(stats.connections == 1U);
    CHECK(stats.activeConnections == 1U);
    CHECK(stats.requestTimeouts == 0U);

    session.shutdown();
    CHECK_FALSE(session.alive());
}

TEST_CASE("invoke() is safe to call from the I/O thread itself", "[integration][client][session]") {
    auto sim = makeSim("reentrant", 2);
    ClientSession session(sessionConfig(sim->udpPort()));
    session.start();

    REQUIRE(waitUntil([&] { return session.listDevices().size() == 1U; }, 3000ms));

    // A handler that already runs on the owner thread must not deadlock in invoke().
    Latch latch;
    session.post([&session, &latch] {
        const std::size_t devices = session.invoke([&] { return session.controller().listDevices().size(); });
        latch.complete(ClientResult<std::string>::ok("devices=" + std::to_string(devices)));
    });

    REQUIRE(waitUntil([&] { return latch.ready(); }, 3000ms));
    auto result = latch.take();
    REQUIRE(result.ok());
    CHECK(result.value() == "devices=1");

    session.shutdown();
}

TEST_CASE("measurements reach the application thread through the notification queue",
          "[integration][client][session]") {
    auto sim = makeSim("streaming", 3);
    ClientSession session(sessionConfig(sim->udpPort()));
    session.start();

    const auto uuid = simUuid(sim->options);
    REQUIRE(waitUntil([&] { return session.listDevices().size() == 1U; }, 3000ms));
    REQUIRE(session.connectDevice(uuid).ok());
    REQUIRE(waitUntil([&] {
        auto summary = session.deviceSummary(uuid);
        return summary.has_value() && summary->state == ConnectionState::Ready;
    },
                      3000ms));

    Latch started;
    session.post([&session, uuid, &started] {
        session.controller().startMeasurementStream(uuid, protocol::StreamMode::Live,
                                                    [&started](ClientResult<std::string> result) {
                                                        started.complete(std::move(result));
                                                    });
    });
    REQUIRE(waitUntil([&] { return started.ready(); }, 3000ms));
    REQUIRE(started.take().ok());

    std::size_t measurements = 0;
    const auto deadline = std::chrono::steady_clock::now() + 2000ms;
    while (std::chrono::steady_clock::now() < deadline && measurements < 5U) {
        uwb::client::ClientNotification note;
        if (session.notifications().waitPop(note, 100ms) && note.kind == NotificationKind::Measurement) {
            CHECK(note.measurement.has_value());
            measurements++;
        }
    }
    CHECK(measurements >= 5U);

    // A slow application thread drops notifications, never frames (§56, §60).
    const auto before = session.diagnostics();
    CHECK(before.txQueueDrops == 0U);
    CHECK(before.requestTimeouts == 0U);

    session.shutdown();
}

TEST_CASE("concurrent commands from the application thread all complete", "[integration][client][session]") {
    auto sim = makeSim("parallel", 4);
    ClientSession session(sessionConfig(sim->udpPort()));
    session.start();

    const auto uuid = simUuid(sim->options);
    REQUIRE(waitUntil([&] { return session.listDevices().size() == 1U; }, 3000ms));
    REQUIRE(session.connectDevice(uuid).ok());
    REQUIRE(waitUntil([&] {
        auto summary = session.deviceSummary(uuid);
        return summary.has_value() && summary->state == ConnectionState::Ready;
    },
                      3000ms));

    static const std::vector<protocol::Did> dids = {
        protocol::Did::DeviceName,
        protocol::Did::EffectiveCapabilities,
        protocol::Did::Uptime,
        protocol::Did::UwbCompleteConfiguration,
    };

    std::vector<std::unique_ptr<ReadLatch>> latches;
    for (std::size_t index = 0; index < dids.size(); ++index) {
        latches.push_back(std::make_unique<ReadLatch>());
        ReadLatch &latch = *latches.back();
        const protocol::Did did = dids[index];
        session.post([&session, uuid, did, &latch] {
            session.controller().readDid(uuid, did, [&latch](ClientResult<ServiceResponse> result) {
                latch.complete(std::move(result));
            });
        });
    }

    std::size_t answered = 0;
    for (auto &latch : latches) {
        if (awaitRead(*latch).has_value()) {
            answered++;
        }
    }
    CHECK(answered == dids.size());

    const auto stats = session.diagnostics();
    CHECK(stats.requestsSent == stats.responsesMatched);
    CHECK(stats.requestTimeouts == 0U);
    CHECK(stats.txQueueDrops == 0U);

    session.shutdown();
}

TEST_CASE("shutdown disconnects every device and post() afterwards is inert", "[integration][client][session]") {
    std::vector<std::unique_ptr<SimulatorFixture>> sims;
    sims.push_back(makeSim("alpha-shutdown", 5));
    sims.push_back(makeSim("bravo-shutdown", 6));

    ControllerConfig config = sessionConfig(sims[0]->udpPort());
    config.discovery.targets = {Endpoint{"127.0.0.1", sims[0]->udpPort()}, Endpoint{"127.0.0.1", sims[1]->udpPort()}};

    ClientSession session(std::move(config));
    session.start();

    REQUIRE(waitUntil([&] { return session.listDevices().size() == 2U; }, 3000ms));
    for (const auto &sim : sims) {
        REQUIRE(session.connectDevice(simUuid(sim->options)).ok());
    }
    REQUIRE(waitUntil([&] {
        const auto devices = session.listDevices();
        return devices.size() == 2U && devices[0].state == ConnectionState::Ready &&
               devices[1].state == ConnectionState::Ready;
    },
                      3000ms));
    CHECK(session.diagnostics().activeConnections == 2U);

    session.shutdown();
    CHECK_FALSE(session.alive());

    // Nothing may be queued or run after shutdown; the façade degrades quietly.
    Latch latch;
    session.post([&latch] { latch.complete(ClientResult<std::string>::ok("must not run")); });
    CHECK_FALSE(latch.ready());
    CHECK(session.listDevices().size() == 2U); // registry survives the shutdown for reporting

    // The simulators must see the connections close, not a dangling client.
    CHECK(waitUntil([&] { return sims[0]->runtimeRef().connectionCount() == 0U; }, 3000ms));
    CHECK(waitUntil([&] { return sims[1]->runtimeRef().connectionCount() == 0U; }, 3000ms));
}

TEST_CASE("a device that vanishes surfaces as a notification, not a stalled thread",
          "[integration][client][session]") {
    auto sim = makeSim("vanishing", 7);
    ClientSession session(sessionConfig(sim->udpPort()));
    session.start();

    const auto uuid = simUuid(sim->options);
    REQUIRE(waitUntil([&] { return session.listDevices().size() == 1U; }, 3000ms));
    REQUIRE(session.connectDevice(uuid).ok());
    REQUIRE(waitUntil([&] {
        auto summary = session.deviceSummary(uuid);
        return summary.has_value() && summary->state == ConnectionState::Ready;
    },
                      3000ms));

    // Remove the device while the application thread is blocked on the queue:
    // the queue must deliver the state change instead of leaving the thread stuck.
    sim.reset();

    bool sawState = false;
    const auto deadline = std::chrono::steady_clock::now() + 4000ms;
    while (std::chrono::steady_clock::now() < deadline && !sawState) {
        uwb::client::ClientNotification note;
        if (!session.notifications().waitPop(note, 100ms)) {
            continue;
        }
        if (note.kind == NotificationKind::DeviceStateChanged && note.device.has_value() &&
            note.device->state != ConnectionState::Ready) {
            sawState = true;
        }
    }
    CHECK(sawState);

    // A command issued after the loss fails with a client error instead of hanging.
    ReadLatch command;
    session.post([&session, uuid, &command] {
        session.controller().readDid(uuid, protocol::Did::Uptime,
                                     [&command](ClientResult<ServiceResponse> result) {
                                         command.complete(std::move(result));
                                     });
    });
    REQUIRE(waitUntil([&] { return command.ready(); }, 4000ms));
    const int code = command.failureCode();
    CHECK(code != 0);
    {
        static const std::vector<int> expected = {
            static_cast<int>(ClientErrorCode::Disconnected),
            static_cast<int>(ClientErrorCode::NotConnected),
            static_cast<int>(ClientErrorCode::NotReady),
            static_cast<int>(ClientErrorCode::TransportFailure),
            static_cast<int>(ClientErrorCode::Timeout),
            static_cast<int>(ClientErrorCode::Cancelled),
        };
        CHECK(std::find(expected.begin(), expected.end(), code) != expected.end());
    }

    session.shutdown();
}
