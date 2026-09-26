#include "uwb/simulator/simulated_backend.hpp"

#include <cstdio>
#include <string>

#include "uwb/protocol/complete_config.hpp"
#include "uwb/protocol/tlv.hpp"
#include "uwb/protocol/wire_writer.hpp"

namespace uwb::simulator {
namespace {

using uwb::protocol::CompleteUwbConfig;
using uwb::protocol::DeviceCalibrationRecord;
using uwb::protocol::DidRecordEntry;
using uwb::protocol::LatestSensorDataRecord;
using uwb::protocol::MeasurementEvent;
using uwb::protocol::MeasurementFlag;
using uwb::protocol::ProtocolErrorCode;
using uwb::protocol::UwbDeviceParametersRecord;
using uwb::protocol::UwbPdoaParametersRecord;
using uwb::protocol::UwbTwrParametersRecord;

ByteBuffer textBytes(std::string_view text) {
    ByteBuffer out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<std::uint8_t>(c));
    }
    return out;
}

std::string textOf(ConstBytes bytes) {
    std::string out;
    out.reserve(bytes.size());
    for (const std::uint8_t byte : bytes) {
        out.push_back(static_cast<char>(byte));
    }
    return out;
}

int nextInt(std::mt19937 &rng, int lowInclusive, int highInclusive) {
    std::uniform_int_distribution<int> dist(lowInclusive, highInclusive);
    return dist(rng);
}

} // namespace

SimulatedUwbBackend::SimulatedUwbBackend(const Settings &settings, SimulatorOptions options) noexcept
    : settings_(settings), options_(std::move(options)), rng_(static_cast<std::uint32_t>(options_.randomSeed)) {
    settings_.queueCapacity = options_.backendCommandQueue;
    faults_.refuseSubmissions = options_.backendRejectsSubmissions;
    faults_.parseErrorNext = options_.backendFaultParseError;
    faults_.timeoutNext = options_.backendFaultTimeout;
    faults_.unsupportedNext = options_.backendFaultUnsupported;
    resetModel();
}

// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------
void SimulatedUwbBackend::setModuleVersion(std::string version) {
    moduleVersion_ = std::move(version);
    dids_[Did::UwbModuleVersion] = uwb::protocol::encodeTextRecord8(textBytes(moduleVersion_));
}

void SimulatedUwbBackend::resetModel() noexcept {
    dids_.clear();

    dids_[Did::UwbModuleVersion] = uwb::protocol::encodeTextRecord8(textBytes(moduleVersion_));

    UwbDeviceParametersRecord device;
    device.id = 1;
    device.role = 1; // anchor
    device.channel = 9;
    device.rate = 1; // 6.8 Mb/s
    dids_[Did::UwbDeviceParameters] = uwb::protocol::encodeRecord(device);

    UwbTwrParametersRecord twr;
    twr.tagCapacity = static_cast<std::uint16_t>(options_.tagCount);
    twr.antennaDelay = 0;
    twr.flags = uwb::protocol::kTwrFlagKalmanEnabled;
    twr.positioningDimension = options_.emitPdoa ? 3 : 2;
    twr.kalmanQ = 1.0F;
    twr.kalmanR = 4.0F;
    twr.correctionParameterA = 1.0F;
    twr.correctionParameterB = 0.0F;
    dids_[Did::UwbTwrParameters] = uwb::protocol::encodeRecord(twr);

    UwbPdoaParametersRecord pdoa;
    pdoa.dlist = 1;
    pdoa.klist = 1;
    pdoa.network = 0x1000U;
    pdoa.anchorId = 1;
    pdoa.rate = 1;
    pdoa.filterEnabled = 1;
    pdoa.userCommand = 0;
    pdoa.pdoaOffsetRaw = 0;
    pdoa.rangeOffsetMm = 0;
    dids_[Did::UwbPdoaParameters] = uwb::protocol::encodeRecord(pdoa);

    dids_[Did::UwbWorkMode] = uwb::protocol::encodeU8Record(1); // TWR
    dids_[Did::UwbMode] = uwb::protocol::encodeU8Record(0);

    std::vector<uwb::protocol::TlvRecord> misc;
    uwb::protocol::TlvRecord vendor;
    vendor.type = 1;
    vendor.value = textBytes("Ai-Thinker BU04");
    misc.push_back(std::move(vendor));
    uwb::protocol::TlvRecord moduleFirmware;
    moduleFirmware.type = 2;
    moduleFirmware.value = textBytes("AT-V1.0.7");
    misc.push_back(std::move(moduleFirmware));
    dids_[Did::UwbMiscellaneousMetadata] = uwb::protocol::encodeTlvRecords(misc);

    DeviceCalibrationRecord calibration;
    dids_[Did::DeviceCalibration] = uwb::protocol::encodeRecord(calibration);

    LatestSensorDataRecord sensor;
    sensor.accX = 0.0F;
    sensor.accY = 0.0F;
    sensor.accZ = 1.0F;
    sensor.angle = 0.0F;
    dids_[Did::UwbLatestSensorData] = uwb::protocol::encodeRecord(sensor);

    dids_[Did::UwbLatestDistance] = uwb::protocol::encodeI32Record(static_cast<std::int32_t>(options_.rangeBaseMm));

    rebuildCompleteConfig();
}

void SimulatedUwbBackend::rebuildCompleteConfig() {
    CompleteUwbConfig config;
    config.schemaVersion = uwb::protocol::kCompleteConfigSchemaVersion1;
    for (const Did member : {Did::UwbDeviceParameters, Did::UwbTwrParameters, Did::UwbPdoaParameters,
                             Did::UwbWorkMode, Did::UwbMode}) {
        DidRecordEntry entry;
        entry.did = static_cast<std::uint16_t>(member);
        entry.data = modelRecord(member);
        config.entries.push_back(std::move(entry));
    }
    dids_[Did::UwbCompleteConfiguration] = uwb::protocol::encodeCompleteUwbConfig(config);
}

void SimulatedUwbBackend::setDid(Did did, ConstBytes record) {
    dids_[did] = ByteBuffer(record.begin(), record.end());
    if (uwb::protocol::isCompleteConfigDid(static_cast<std::uint16_t>(did))) {
        rebuildCompleteConfig();
    }
}

ByteBuffer SimulatedUwbBackend::modelRecord(Did did) const {
    const auto it = dids_.find(did);
    return it != dids_.end() ? it->second : ByteBuffer{};
}

// ---------------------------------------------------------------------------
// Submission (§48.2 one outstanding exchange, §48.3/§53 bounded queue)
// ---------------------------------------------------------------------------
Result<OperationId> SimulatedUwbBackend::enqueue(Operation op) {
    if (faults_.refuseSubmissions || queue_.size() >= settings_.queueCapacity) {
        return Result<OperationId>::error(ProtocolErrorCode::UnsupportedService);
    }
    op.id = nextId_++;
    const OperationId id = op.id;
    queue_.push_back(std::move(op));
    return Result<OperationId>::ok(id);
}

Result<OperationId> SimulatedUwbBackend::submitAt(std::uint32_t owner, ConstBytes command, std::uint32_t timeoutMs,
                                                  AtCompletion done) {
    Operation op;
    op.owner = owner;
    op.kind = Kind::At;
    op.payload = ByteBuffer(command.begin(), command.end());
    op.deadlineOverrideMs = timeoutMs;
    op.atDone = std::move(done);
    return enqueue(std::move(op));
}

Result<OperationId> SimulatedUwbBackend::readDid(std::uint32_t owner, Did did, DidReadCompletion done) {
    Operation op;
    op.owner = owner;
    op.kind = Kind::DidRead;
    op.did = did;
    op.readDone = std::move(done);
    return enqueue(std::move(op));
}

Result<OperationId> SimulatedUwbBackend::writeDid(std::uint32_t owner, Did did, ConstBytes record,
                                                  DidWriteCompletion done) {
    Operation op;
    op.owner = owner;
    op.kind = Kind::DidWrite;
    op.did = did;
    op.payload = ByteBuffer(record.begin(), record.end());
    op.writeDone = std::move(done);
    return enqueue(std::move(op));
}

Result<OperationId> SimulatedUwbBackend::startRoutine(std::uint32_t owner, RoutineId routine, ConstBytes options,
                                                      RoutineCompletion done) {
    Operation op;
    op.owner = owner;
    op.kind = Kind::Routine;
    op.routine = routine;
    op.payload = ByteBuffer(options.begin(), options.end());
    op.routineDone = std::move(done);
    return enqueue(std::move(op));
}

bool SimulatedUwbBackend::requestRoutineCancel(OperationId operation) noexcept {
    for (auto &op : queue_) {
        if (op.id == operation) {
            op.cancelled = true;
            return true;
        }
    }
    return false;
}

void SimulatedUwbBackend::cancelOperations(std::uint32_t owner) noexcept {
    for (auto &op : queue_) {
        if (op.owner == owner) {
            op.cancelled = true;
        }
    }
}

bool SimulatedUwbBackend::busy() const noexcept {
    return faults_.refuseSubmissions || queue_.size() >= settings_.queueCapacity;
}

// ---------------------------------------------------------------------------
// tick(): advance the simulated UART line
// ---------------------------------------------------------------------------
void SimulatedUwbBackend::tick(std::uint64_t nowUs) noexcept {
    if (queue_.empty()) {
        return;
    }

    const std::uint64_t gapUs = static_cast<std::uint64_t>(settings_.interCommandGapMs) * 1000ULL;
    if (!inFlight_) {
        if (lastCompletionUs_ != 0 && nowUs < lastCompletionUs_ + gapUs) {
            return; // §48.2 inter-command gap
        }
        auto &pending = queue_.front();
        const std::uint32_t timeoutMs =
            pending.deadlineOverrideMs != 0U ? pending.deadlineOverrideMs : settings_.commandTimeoutMs;
        pending.dueUs = nowUs + (static_cast<std::uint64_t>(settings_.latencyMs) + faults_.extraLatencyMs) * 1000ULL;
        pending.deadlineUs = nowUs + static_cast<std::uint64_t>(timeoutMs) * 1000ULL;
        inFlight_ = true;
    }

    const Operation &op = queue_.front();
    if (op.cancelled) {
        finish(nowUs, BackendStatus::Cancelled, {});
        return;
    }
    if (faults_.timeoutNext) {
        if (nowUs >= op.deadlineUs) {
            faults_.timeoutNext = false;
            finish(nowUs, BackendStatus::Timeout, {});
        }
        return;
    }
    if (nowUs < op.dueUs) {
        return;
    }

    ByteBuffer data;
    BackendStatus status = BackendStatus::Completed;
    switch (op.kind) {
    case Kind::At:
        if (faults_.parseErrorNext) {
            faults_.parseErrorNext = false;
            status = BackendStatus::ParseError;
            data = textBytes("\r\n");
        } else if (faults_.unsupportedNext) {
            faults_.unsupportedNext = false;
            status = BackendStatus::Unsupported;
        } else {
            data = atResponse(textOf(op.payload));
        }
        break;
    case Kind::DidRead:
        if (faults_.parseErrorNext) {
            faults_.parseErrorNext = false;
            status = BackendStatus::ParseError;
        } else if (faults_.unsupportedNext) {
            faults_.unsupportedNext = false;
            status = BackendStatus::Unsupported;
        } else {
            data = modelRecord(op.did);
            if (data.empty()) {
                status = BackendStatus::Failed;
            }
        }
        break;
    case Kind::DidWrite:
        applyWrite(op);
        break;
    case Kind::Routine:
        applyRoutine(op, data, status);
        break;
    }
    finish(nowUs, status, std::move(data));
}

void SimulatedUwbBackend::finish(std::uint64_t nowUs, BackendStatus status, ByteBuffer data) noexcept {
    Operation op = std::move(queue_.front());
    queue_.pop_front();
    inFlight_ = false;
    lastCompletionUs_ = nowUs;

    switch (op.kind) {
    case Kind::At:
        if (op.atDone) {
            op.atDone(uwb::server::AtResult{status, std::move(data)});
        }
        break;
    case Kind::DidRead:
        if (op.readDone) {
            op.readDone(uwb::server::DidReadResult{status, std::move(data)});
        }
        break;
    case Kind::DidWrite:
        if (op.writeDone) {
            op.writeDone(uwb::server::DidWriteResult{status});
        }
        break;
    case Kind::Routine:
        if (op.routineDone) {
            op.routineDone(uwb::server::RoutineResult{status, std::move(data)});
        }
        break;
    }
}

void SimulatedUwbBackend::applyWrite(const Operation &op) noexcept {
    if (op.did == Did::UwbCompleteConfiguration) {
        auto decoded = uwb::protocol::decodeCompleteUwbConfig(op.payload);
        if (!decoded.ok()) {
            return; // rejected upstream by DidService in practice; keep the model intact
        }
        for (const DidRecordEntry &entry : decoded.value().entries) {
            dids_[entry.didId()] = entry.data;
        }
        rebuildCompleteConfig();
        return;
    }
    if (op.did == Did::UwbModuleVersion) {
        auto decoded = uwb::protocol::decodeTextRecord8(op.payload, uwb::protocol::kMaxDeviceNameLength);
        if (decoded.ok()) {
            moduleVersion_.assign(decoded.value().begin(), decoded.value().end());
        }
    }
    dids_[op.did] = op.payload;
    if (uwb::protocol::isCompleteConfigDid(static_cast<std::uint16_t>(op.did))) {
        rebuildCompleteConfig();
    }
}

void SimulatedUwbBackend::applyRoutine(const Operation &op, ByteBuffer &data, BackendStatus &status) noexcept {
    switch (op.routine) {
    case RoutineId::SaveUwbConfiguration:
        ++saveCount_;
        break;
    case RoutineId::RestartUwbModule:
        ++restartCount_;
        break;
    case RoutineId::RestoreUwbDefaults:
        ++restoreCount_;
        resetModel();
        break;
    case RoutineId::UwbMeasurementAcquisition: {
        // Option byte: 0x00 stop, 0x01 (or empty) start (§28.2).
        const bool stop = !op.payload.empty() && op.payload[0] == 0x00U;
        if (stop) {
            ++measurementStopCount_;
            measurementEnabled_ = false;
        } else {
            ++measurementStartCount_;
            measurementEnabled_ = true;
        }
        break;
    }
    case RoutineId::AddTag:
    case RoutineId::DeleteTag: {
        uwb::protocol::WireWriter writer(data);
        writer.writeU16(static_cast<std::uint16_t>(options_.tagCount));
        break;
    }
    case RoutineId::InitializeUwbBackend:
    case RoutineId::DeviceCalibration:
    case RoutineId::TestLed:
    case RoutineId::TestOled:
        break;
    default:
        status = BackendStatus::Unsupported;
        break;
    }
}

// ---------------------------------------------------------------------------
// AT text simulation (§30, §48.1)
// ---------------------------------------------------------------------------
ByteBuffer SimulatedUwbBackend::atResponse(std::string_view command) const {
    ByteBuffer out;
    const auto append = [&](std::string_view text) {
        for (const char c : text) {
            out.push_back(static_cast<std::uint8_t>(c));
        }
    };

    if (command.rfind("AT+GETVER", 0) == 0) {
        append(" software:");
        append(moduleVersion_);
        append("\r\nOK\r\n");
    } else if (command.rfind("AT+DISTANCE", 0) == 0) {
        char buffer[32];
        std::snprintf(buffer, sizeof buffer, " %.2f", static_cast<double>(options_.rangeBaseMm) / 1000.0);
        append(buffer);
        append("\r\nOK\r\n");
    } else if (command.rfind("AT", 0) == 0) {
        append("\r\nOK\r\n");
    } else {
        append("ERR\r\n");
    }
    return out;
}

// ---------------------------------------------------------------------------
// Measurement generation (§34, §35)
// ---------------------------------------------------------------------------
bool SimulatedUwbBackend::measurementDue(std::uint64_t nowUs) const noexcept {
    if (!measurementEnabled_ || options_.measurementPeriodMs == 0U) {
        return false;
    }
    const std::uint64_t periodUs = static_cast<std::uint64_t>(options_.measurementPeriodMs) * 1000ULL;
    return lastMeasurementUs_ == 0 || nowUs >= lastMeasurementUs_ + periodUs;
}

std::uint16_t SimulatedUwbBackend::nextTagId() const noexcept {
    const std::uint32_t tags = options_.tagCount == 0U ? 1U : options_.tagCount;
    return static_cast<std::uint16_t>(1U + (measurementSequence_ % tags));
}

MeasurementEvent SimulatedUwbBackend::nextMeasurement(std::uint64_t nowUs) {
    MeasurementEvent event;
    event.networkId = 0x1000;
    event.anchorId = 1;
    event.tagId = nextTagId();
    ++measurementSequence_;

    const int noise = static_cast<int>(options_.rangeNoiseMm);
    const int raw = static_cast<int>(options_.rangeBaseMm) + nextInt(rng_, -noise, noise);
    event.rawRangeMm = raw;
    event.correctedRangeMm = raw + nextInt(rng_, -5, 5);

    event.flags = MeasurementFlag::RawRangeValid | MeasurementFlag::CorrectedRangeValid |
                  MeasurementFlag::QualityValid | MeasurementFlag::NetworkIdValid | MeasurementFlag::AnchorIdValid |
                  MeasurementFlag::TagIdValid;

    if (options_.emitPdoa) {
        const int azimuth = nextInt(rng_, -90000, 90000);
        const int elevation = nextInt(rng_, -30000, 30000);
        event.rawAzimuthMilliDeg = azimuth;
        event.correctedAzimuthMilliDeg = azimuth + nextInt(rng_, -200, 200);
        event.rawElevationMilliDeg = elevation;
        event.correctedElevationMilliDeg = elevation + nextInt(rng_, -100, 100);
        event.flags = event.flags | MeasurementFlag::RawAzimuthValid | MeasurementFlag::CorrectedAzimuthValid |
                      MeasurementFlag::RawElevationValid | MeasurementFlag::CorrectedElevationValid;
    }

    event.quality = static_cast<std::uint16_t>(nextInt(rng_, 700, 1000));
    event.reserved = 0;

    lastMeasurementUs_ = nowUs;
    dids_[Did::UwbLatestDistance] = uwb::protocol::encodeI32Record(event.correctedRangeMm);
    return event;
}

uwb::protocol::LocalPositionEvent SimulatedUwbBackend::nextLocalPosition(std::uint64_t nowUs) {
    (void)nowUs;
    uwb::protocol::LocalPositionEvent event;
    event.networkId = 0x1000;
    event.tagId = nextTagId();
    event.xMm = static_cast<std::int32_t>(options_.rangeBaseMm) + nextInt(rng_, -200, 200);
    event.yMm = static_cast<std::int32_t>(nextInt(rng_, -500, 500));
    event.zMm = static_cast<std::int32_t>(nextInt(rng_, -100, 100));
    event.quality = static_cast<std::uint16_t>(nextInt(rng_, 600, 1000));
    event.flags = 0;
    return event;
}

} // namespace uwb::simulator
