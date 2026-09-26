#include <array>

#include <catch2/catch_all.hpp>

#include "uwb/protocol/routines.hpp"
#include "uwb/server/routine_service.hpp"
#include "uwb/server/server_errors.hpp"

#include "server_fakes.hpp"

using uwb::protocol::RoutineId;
using uwb::protocol::RoutineState;
using uwb::server::BackendStatus;
using uwb::test::FakeBackend;
using uwb::server::OperationId;
using uwb::server::RoutineService;
using uwb::server::ServerStatus;

namespace {

constexpr uwb::server::ConnectionId kOwner = 7;
constexpr uwb::server::ConnectionId kOther = 9;

} // namespace

// ---------------------------------------------------------------------------
// §42 policy tables
// ---------------------------------------------------------------------------

TEST_CASE("routine cancellability follows the shared routine table", "[unit][routine][policy]") {
    // §42: acquisition, save and test routines are cancellable; tag maintenance
    // and configuration-critical routines are not.
    CHECK(RoutineService::routineCancellable(RoutineId::InitializeUwbBackend));
    CHECK(RoutineService::routineCancellable(RoutineId::UwbMeasurementAcquisition));
    CHECK(RoutineService::routineCancellable(RoutineId::TestLed));
    CHECK(RoutineService::routineCancellable(RoutineId::TestOled));
    CHECK_FALSE(RoutineService::routineCancellable(RoutineId::AddTag));
    CHECK_FALSE(RoutineService::routineCancellable(RoutineId::DeleteTag));
    CHECK_FALSE(RoutineService::routineCancellable(RoutineId::SaveUwbConfiguration));
    CHECK_FALSE(RoutineService::routineCancellable(RoutineId::RestoreUwbDefaults));
    CHECK_FALSE(RoutineService::routineCancellable(RoutineId::RestartUwbModule));
}

TEST_CASE("only the backend initialization routine is server-local", "[unit][routine][policy]") {
    CHECK(RoutineService::routineIsLocal(RoutineId::InitializeUwbBackend));
    CHECK_FALSE(RoutineService::routineIsLocal(RoutineId::AddTag));
    CHECK_FALSE(RoutineService::routineIsLocal(RoutineId::UwbMeasurementAcquisition));
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

TEST_CASE("starting a local routine reports Running and asks for backend re-init", "[unit][routine]") {
    RoutineService routines;

    auto started = routines.start(RoutineId::InitializeUwbBackend, kOwner, {});
    REQUIRE(started.ok());
    CHECK(started.value.state == RoutineState::Running);
    CHECK_FALSE(started.value.needsBackend);
    CHECK(started.value.cancellable);
    CHECK(routines.runningCount() == 1);

    const RoutineService::LocalEffect effect = routines.takeEffect(RoutineId::InitializeUwbBackend);
    CHECK(effect.reinitBackend);
    CHECK(routines.find(RoutineId::InitializeUwbBackend, kOwner) == nullptr);
}

TEST_CASE("starting a backend routine requires an IUwbBackend submission", "[unit][routine]") {
    RoutineService routines;
    FakeBackend backend; // the dispatcher, not RoutineService, submits the operation

    auto started = routines.start(RoutineId::UwbMeasurementAcquisition, kOwner, {});
    REQUIRE(started.ok());
    CHECK(started.value.needsBackend);
    CHECK(routines.findByOperation(started.value.backendOperation) == nullptr);

    // The dispatcher submits the operation and records the backend handle.
    routines.noteBackendOperation(started.value.handle, static_cast<OperationId>(42));
    const RoutineService::Routine *entry = routines.find(RoutineId::UwbMeasurementAcquisition, kOwner);
    REQUIRE(entry != nullptr);
    CHECK(entry->backendOperation == static_cast<OperationId>(42));
    CHECK(routines.findByOperation(static_cast<OperationId>(42)) != nullptr);

    const std::array<std::uint8_t, 3> statusRecord{1, 2, 3};
    routines.completeBackendOperation(static_cast<OperationId>(42), BackendStatus::Completed,
                                      uwb::protocol::ConstBytes{statusRecord.data(), statusRecord.size()});
    const RoutineService::Routine *done = routines.find(RoutineId::UwbMeasurementAcquisition, kOwner);
    REQUIRE(done != nullptr);
    CHECK(done->state == RoutineState::Completed);
    CHECK(done->backendOperation == uwb::server::kInvalidOperationId);
    CHECK(done->statusRecord.size() == 3);
    CHECK(routines.runningCount() == 0);
    CHECK(backend.submissions_.empty());
}

TEST_CASE("a finished routine can be started again", "[unit][routine]") {
    RoutineService routines;

    auto first = routines.start(RoutineId::TestLed, kOwner, {});
    REQUIRE(first.ok());
    routines.noteBackendOperation(first.value.handle, static_cast<OperationId>(11));
    routines.completeBackendOperation(static_cast<OperationId>(11), BackendStatus::Failed, {});

    // §28: StartRoutine after a Completed/Failed run starts a new instance.
    auto again = routines.start(RoutineId::TestLed, kOwner, {});
    REQUIRE(again.ok());
    CHECK(again.value.state == RoutineState::Running);
    CHECK(again.value.handle != first.value.handle);
    CHECK(routines.count() == 1);
}

TEST_CASE("starting the same routine twice for one owner is refused", "[unit][routine]") {
    RoutineService routines;

    REQUIRE(routines.start(RoutineId::UwbMeasurementAcquisition, kOwner, {}).ok());
    const auto second = routines.start(RoutineId::UwbMeasurementAcquisition, kOwner, {});
    CHECK(second.status == ServerStatus::RoutineAlreadyRunning);

    // Another connection may run the same routine independently (§42 owner scope).
    CHECK(routines.start(RoutineId::UwbMeasurementAcquisition, kOther, {}).ok());
    CHECK(routines.runningCount() == 2);
}

TEST_CASE("an unknown routine id is refused", "[unit][routine]") {
    RoutineService routines;
    const auto started = routines.start(static_cast<RoutineId>(0x02FF), kOwner, {});
    CHECK(started.status == ServerStatus::UnknownRoutine);
    CHECK(routines.count() == 0);
}

// ---------------------------------------------------------------------------
// Stop and cancel (§28)
// ---------------------------------------------------------------------------

TEST_CASE("stopping a cancellable routine moves it to Stopping and cancels the backend", "[unit][routine]") {
    RoutineService routines;
    FakeBackend backend;

    auto started = routines.start(RoutineId::UwbMeasurementAcquisition, kOwner, {});
    REQUIRE(started.ok());
    const auto submitted = backend.startRoutine(kOwner, RoutineId::UwbMeasurementAcquisition, {}, {});
    REQUIRE(submitted.ok());
    routines.noteBackendOperation(started.value.handle, submitted.value());

    auto stopped = routines.stop(RoutineId::UwbMeasurementAcquisition, kOwner, &backend);
    REQUIRE(stopped.ok());
    CHECK(stopped.value.state == RoutineState::Stopping);
    CHECK(stopped.value.cancelRequested);
    CHECK(routines.runningCount() == 1); // still stopping until the backend confirms

    auto results = routines.requestResults(RoutineId::UwbMeasurementAcquisition, kOwner, &backend);
    REQUIRE(results.ok());
    CHECK(results.value.state == RoutineState::Stopping);

    CHECK(backend.find(0)->cancelled);
    routines.completeBackendOperation(submitted.value(), BackendStatus::Cancelled, {});
    const RoutineService::Routine *done = routines.find(RoutineId::UwbMeasurementAcquisition, kOwner);
    REQUIRE(done != nullptr);
    CHECK(done->state == RoutineState::Cancelled);
    CHECK(routines.runningCount() == 0);
}

TEST_CASE("stopping a non-cancellable routine is reported as not cancellable", "[unit][routine]") {
    RoutineService routines;
    FakeBackend backend;

    auto started = routines.start(RoutineId::AddTag, kOwner, {});
    REQUIRE(started.ok());
    CHECK_FALSE(started.value.cancellable);
    routines.noteBackendOperation(started.value.handle, static_cast<OperationId>(31));

    const auto stopped = routines.stop(RoutineId::AddTag, kOwner, &backend);
    CHECK(stopped.status == ServerStatus::RoutineNotCancellable);

    // The routine keeps running: refusing Stop must not fake a cancellation.
    const RoutineService::Routine *entry = routines.find(RoutineId::AddTag, kOwner);
    REQUIRE(entry != nullptr);
    CHECK(entry->state == RoutineState::Running);
    CHECK_FALSE(entry->cancelRequested);
}

TEST_CASE("stopping an unknown or idle routine is reported as not running", "[unit][routine]") {
    RoutineService routines;
    FakeBackend backend;

    CHECK(routines.stop(RoutineId::DeleteTag, kOwner, &backend).status == ServerStatus::RoutineNotRunning);
    CHECK(routines.stop(static_cast<RoutineId>(0x02FF), kOwner, &backend).status == ServerStatus::UnknownRoutine);

    // requestResults for something that never ran reports Idle (§28 state table).
    auto results = routines.requestResults(RoutineId::DeleteTag, kOwner, &backend);
    REQUIRE(results.ok());
    CHECK(results.value.state == RoutineState::Idle);
}

TEST_CASE("requestCancel only cancels cancellable running routines", "[unit][routine]") {
    RoutineService routines;

    REQUIRE(routines.start(RoutineId::UwbMeasurementAcquisition, kOwner, {}).ok());
    REQUIRE(routines.start(RoutineId::SaveUwbConfiguration, kOwner, {}).ok());

    CHECK(routines.requestCancel(RoutineId::UwbMeasurementAcquisition, kOwner));
    CHECK_FALSE(routines.requestCancel(RoutineId::SaveUwbConfiguration, kOwner));
    CHECK_FALSE(routines.requestCancel(RoutineId::TestOled, kOwner));

    const RoutineService::Routine *acquisition = routines.find(RoutineId::UwbMeasurementAcquisition, kOwner);
    REQUIRE(acquisition != nullptr);
    CHECK(acquisition->state == RoutineState::Stopping);
}

TEST_CASE("a failed backend submission marks the routine failed", "[unit][routine]") {
    RoutineService routines;

    auto started = routines.start(RoutineId::DeviceCalibration, kOwner, {});
    REQUIRE(started.ok());
    routines.noteSubmitFailed(started.value.handle);

    const RoutineService::Routine *entry = routines.find(RoutineId::DeviceCalibration, kOwner);
    REQUIRE(entry != nullptr);
    CHECK(entry->state == RoutineState::Failed);
    CHECK(entry->backendOperation == uwb::server::kInvalidOperationId);
}

// ---------------------------------------------------------------------------
// §9.4 per-connection cleanup
// ---------------------------------------------------------------------------

TEST_CASE("disconnecting a connection stops the routines it owns", "[unit][routine]") {
    RoutineService routines;

    REQUIRE(routines.start(RoutineId::UwbMeasurementAcquisition, kOwner, {}).ok());
    REQUIRE(routines.start(RoutineId::AddTag, kOwner, {}).ok());
    REQUIRE(routines.start(RoutineId::UwbMeasurementAcquisition, kOther, {}).ok());

    const std::size_t stopped = routines.stopAllForConnection(kOwner);
    CHECK(stopped == 2);
    CHECK(routines.count() == 1);
    CHECK(routines.find(RoutineId::UwbMeasurementAcquisition, kOwner) == nullptr);
    CHECK(routines.find(RoutineId::AddTag, kOwner) == nullptr);
    CHECK(routines.find(RoutineId::UwbMeasurementAcquisition, kOther) != nullptr);
}

TEST_CASE("completing an unknown backend operation is ignored", "[unit][routine]") {
    RoutineService routines;
    REQUIRE(routines.start(RoutineId::TestOled, kOwner, {}).ok());
    routines.completeBackendOperation(static_cast<OperationId>(9999), BackendStatus::Completed, {});
    const RoutineService::Routine *entry = routines.find(RoutineId::TestOled, kOwner);
    REQUIRE(entry != nullptr);
    CHECK(entry->state == RoutineState::Running);
    CHECK(entry->statusRecord.empty());
}
