#pragma once

#include <cstdint>
#include <deque>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>

#include "uwb/protocol/did_records.hpp"
#include "uwb/protocol/dids.hpp"
#include "uwb/protocol/events.hpp"
#include "uwb/protocol/result.hpp"
#include "uwb/protocol/routines.hpp"
#include "uwb/protocol/services.hpp"
#include "uwb/simulator/simulator_config.hpp"
#include "uwb/server/uwb_backend.hpp"

namespace uwb::simulator {

using uwb::protocol::ByteBuffer;
using uwb::protocol::ConstBytes;
using uwb::protocol::Did;
using uwb::protocol::Result;
using uwb::protocol::RoutineId;
using uwb::server::AtCompletion;
using uwb::server::BackendStatus;
using uwb::server::DidReadCompletion;
using uwb::server::DidWriteCompletion;
using uwb::server::IUwbBackend;
using uwb::server::OperationId;
using uwb::server::RoutineCompletion;

// Configurable fake UWB backend (implementation plan §22.2, §22.4).
//
// It models the solicited-command rules of specification §48: one outstanding
// solicited exchange at a time, a bounded command queue, per-command timeout,
// optional inter-command gap, and injectable fault classes (parse error, timeout,
// unsupported, busy). Completions fire only from tick(), mirroring the
// single-threaded poll loop used by the firmware.
class SimulatedUwbBackend final : public IUwbBackend {
public:
    struct Settings {
        std::uint32_t latencyMs = 5;
        std::uint32_t interCommandGapMs = 0;
        std::uint32_t commandTimeoutMs = 1000;
        std::uint32_t queueCapacity = uwb::protocol::kUwbCommandQueueCapacity;
    };

    // One-shot fault injection knobs (implementation plan §22.4).
    struct Faults {
        bool parseErrorNext = false;    // next command answers with a malformed response
        bool timeoutNext = false;       // next command never answers (expires at p2*)
        bool unsupportedNext = false;   // next command reports "not supported"
        bool refuseSubmissions = false; // command queue is full -> NRC 0x21
        std::uint32_t extraLatencyMs = 0;
    };

    SimulatedUwbBackend(const Settings &settings, SimulatorOptions options) noexcept;

    // --- IUwbBackend ---------------------------------------------------------
    [[nodiscard]] Result<OperationId> submitAt(std::uint32_t owner, ConstBytes command, std::uint32_t timeoutMs,
                                               AtCompletion done) override;
    [[nodiscard]] Result<OperationId> readDid(std::uint32_t owner, Did did, DidReadCompletion done) override;
    [[nodiscard]] Result<OperationId> writeDid(std::uint32_t owner, Did did, ConstBytes record,
                                               DidWriteCompletion done) override;
    [[nodiscard]] Result<OperationId> startRoutine(std::uint32_t owner, RoutineId routine, ConstBytes options,
                                                   RoutineCompletion done) override;
    [[nodiscard]] bool requestRoutineCancel(OperationId operation) noexcept override;
    void cancelOperations(std::uint32_t owner) noexcept override;
    void tick(std::uint64_t nowUs) noexcept override;
    [[nodiscard]] bool busy() const noexcept override;
    [[nodiscard]] std::size_t pendingOperations() const noexcept override { return queue_.size(); }

    // --- simulated UWB model -------------------------------------------------
    [[nodiscard]] std::unordered_map<Did, ByteBuffer> &dids() noexcept { return dids_; }
    [[nodiscard]] const std::unordered_map<Did, ByteBuffer> &dids() const noexcept { return dids_; }
    void setDid(Did did, ConstBytes record);
    void resetModel() noexcept;

    [[nodiscard]] const Faults &faults() const noexcept { return faults_; }
    [[nodiscard]] Faults &faults() noexcept { return faults_; }

    [[nodiscard]] const std::string &moduleVersion() const noexcept { return moduleVersion_; }
    void setModuleVersion(std::string version);

    [[nodiscard]] std::size_t saveCount() const noexcept { return saveCount_; }
    [[nodiscard]] std::size_t restartCount() const noexcept { return restartCount_; }
    [[nodiscard]] std::size_t restoreCount() const noexcept { return restoreCount_; }
    [[nodiscard]] std::size_t measurementStartCount() const noexcept { return measurementStartCount_; }
    [[nodiscard]] std::size_t measurementStopCount() const noexcept { return measurementStopCount_; }
    [[nodiscard]] bool acquisitionActive() const noexcept { return measurementStartCount_ > measurementStopCount_; }

    // Measurement generation (specification §34, §35).
    [[nodiscard]] bool measurementDue(std::uint64_t nowUs) const noexcept;
    [[nodiscard]] uwb::protocol::MeasurementEvent nextMeasurement(std::uint64_t nowUs);
    [[nodiscard]] uwb::protocol::LocalPositionEvent nextLocalPosition(std::uint64_t nowUs);
    void setMeasurementEnabled(bool enabled) noexcept { measurementEnabled_ = enabled; }

    // Deterministic AT text response for raw AT service tests.
    [[nodiscard]] ByteBuffer atResponse(std::string_view command) const;

private:
    enum class Kind : std::uint8_t { At = 0, DidRead = 1, DidWrite = 2, Routine = 3 };

    struct Operation {
        OperationId id = 0;
        std::uint32_t owner = 0;
        Kind kind = Kind::At;
        Did did = Did::DeviceUuid;
        RoutineId routine = static_cast<RoutineId>(0);
        ByteBuffer payload; // AT text, DID record, or routine options
        std::uint32_t deadlineOverrideMs = 0;
        std::uint64_t dueUs = 0;
        std::uint64_t deadlineUs = 0;
        bool cancelled = false;
        AtCompletion atDone;
        DidReadCompletion readDone;
        DidWriteCompletion writeDone;
        RoutineCompletion routineDone;
    };

    [[nodiscard]] Result<OperationId> enqueue(Operation op);
    void finish(std::uint64_t nowUs, BackendStatus status, ByteBuffer data) noexcept;
    void rebuildCompleteConfig();
    void applyWrite(const Operation &op) noexcept;
    void applyRoutine(const Operation &op, ByteBuffer &data, BackendStatus &status) noexcept;
    [[nodiscard]] ByteBuffer modelRecord(Did did) const;
    [[nodiscard]] std::uint16_t nextTagId() const noexcept;

    Settings settings_;
    SimulatorOptions options_;
    Faults faults_;

    // The queue head is the command currently on the simulated UART line
    // (specification §48.2: exactly one solicited exchange at a time).
    std::deque<Operation> queue_;
    bool inFlight_ = false;
    OperationId nextId_ = 1;
    std::uint64_t lastCompletionUs_ = 0;

    std::unordered_map<Did, ByteBuffer> dids_;
    std::string moduleVersion_ = "V1.0.7";

    std::size_t saveCount_ = 0;
    std::size_t restartCount_ = 0;
    std::size_t restoreCount_ = 0;
    std::size_t measurementStartCount_ = 0;
    std::size_t measurementStopCount_ = 0;

    bool measurementEnabled_ = true;
    std::uint32_t measurementSequence_ = 0;
    std::uint64_t lastMeasurementUs_ = 0;
    std::mt19937 rng_;
};

} // namespace uwb::simulator
