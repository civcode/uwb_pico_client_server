#include "uwb/domain/measurement.hpp"

namespace uwb::domain {

namespace {

using uwb::protocol::MeasurementFlag;
using uwb::protocol::MeasurementFlags;

[[nodiscard]] std::optional<std::int32_t> valueIf(std::int32_t value, MeasurementFlags flags,
                                                  MeasurementFlag flag) noexcept {
    if (uwb::protocol::hasFlag(flags, flag)) {
        return value;
    }
    return std::nullopt;
}

} // namespace

RangeAngleMeasurement normalizeMeasurement(const DeviceUuid &gatewayUuid,
                                           const uwb::protocol::EventNotification &event,
                                           const uwb::protocol::MeasurementEvent &measurement,
                                           std::uint64_t hostReceiveTimestampUs) noexcept {
    RangeAngleMeasurement out;
    out.gatewayUuid = gatewayUuid;

    out.networkId = measurement.networkId;
    out.anchorId = measurement.anchorId;
    out.tagId = measurement.tagId;

    out.sequence = event.sequence;
    out.deviceTimestampUs = event.timestampUs;
    out.hostReceiveTimestampUs = hostReceiveTimestampUs;

    const MeasurementFlags flags = measurement.flags;
    out.rawRangeMm = valueIf(measurement.rawRangeMm, flags, MeasurementFlag::RawRangeValid);
    out.correctedRangeMm = valueIf(measurement.correctedRangeMm, flags, MeasurementFlag::CorrectedRangeValid);
    out.rawAzimuthMilliDeg = valueIf(measurement.rawAzimuthMilliDeg, flags, MeasurementFlag::RawAzimuthValid);
    out.correctedAzimuthMilliDeg =
        valueIf(measurement.correctedAzimuthMilliDeg, flags, MeasurementFlag::CorrectedAzimuthValid);
    out.rawElevationMilliDeg = valueIf(measurement.rawElevationMilliDeg, flags, MeasurementFlag::RawElevationValid);
    out.correctedElevationMilliDeg =
        valueIf(measurement.correctedElevationMilliDeg, flags, MeasurementFlag::CorrectedElevationValid);

    if (uwb::protocol::hasFlag(flags, MeasurementFlag::QualityValid)) {
        out.quality = measurement.quality;
    }

    out.flags = flags;
    out.streamId = event.streamId;
    return out;
}

LocalPosition normalizeLocalPosition(const DeviceUuid &gatewayUuid,
                                     const uwb::protocol::EventNotification &event,
                                     const uwb::protocol::LocalPositionEvent &position,
                                     std::uint64_t hostReceiveTimestampUs) noexcept {
    LocalPosition out;
    out.gatewayUuid = gatewayUuid;
    out.networkId = position.networkId;
    out.tagId = position.tagId;
    out.xMm = position.xMm;
    out.yMm = position.yMm;
    out.zMm = position.zMm;
    out.sequence = event.sequence;
    out.deviceTimestampUs = event.timestampUs;
    out.hostReceiveTimestampUs = hostReceiveTimestampUs;
    out.quality = position.quality;
    out.flags = position.flags;
    out.streamId = event.streamId;
    return out;
}

} // namespace uwb::domain
