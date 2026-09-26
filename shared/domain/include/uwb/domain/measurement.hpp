#pragma once

#include <cstdint>
#include <optional>

#include "uwb/domain/device_identity.hpp"
#include "uwb/protocol/events.hpp"

namespace uwb::domain {

// Decoded network events are converted exactly once into this transport/UI
// independent object (specification §61). Downstream code SHALL NOT parse wire
// packets again.
struct RangeAngleMeasurement {
    DeviceUuid gatewayUuid;

    std::uint16_t networkId = 0;
    std::uint16_t anchorId = 0; // source node (anchor)
    std::uint16_t tagId = 0;    // target node (tag)

    std::uint32_t sequence = 0;
    std::uint64_t deviceTimestampUs = 0;
    std::uint64_t hostReceiveTimestampUs = 0;

    std::optional<std::int32_t> rawRangeMm;
    std::optional<std::int32_t> correctedRangeMm;
    std::optional<std::int32_t> rawAzimuthMilliDeg;
    std::optional<std::int32_t> correctedAzimuthMilliDeg;
    std::optional<std::int32_t> rawElevationMilliDeg;
    std::optional<std::int32_t> correctedElevationMilliDeg;
    std::optional<std::uint16_t> quality;

    // Diagnostics kept for display/logging; not used by processing code.
    uwb::protocol::MeasurementFlags flags = 0;
    std::uint16_t streamId = 0;
};

struct LocalPosition {
    DeviceUuid gatewayUuid;

    std::uint16_t networkId = 0;
    std::uint16_t tagId = 0;

    std::int32_t xMm = 0;
    std::int32_t yMm = 0;
    std::int32_t zMm = 0;

    std::uint32_t sequence = 0;
    std::uint64_t deviceTimestampUs = 0;
    std::uint64_t hostReceiveTimestampUs = 0;

    std::optional<std::uint16_t> quality;
    std::uint16_t flags = 0;
    std::uint16_t streamId = 0;
};

// Field validity follows the §34 flag bits: a field is only present when its
// valid bit is set, so downstream code can rely on std::optional instead of
// sentinel values.
[[nodiscard]] RangeAngleMeasurement normalizeMeasurement(const DeviceUuid &gatewayUuid,
                                                         const uwb::protocol::EventNotification &event,
                                                         const uwb::protocol::MeasurementEvent &measurement,
                                                         std::uint64_t hostReceiveTimestampUs) noexcept;

[[nodiscard]] LocalPosition normalizeLocalPosition(const DeviceUuid &gatewayUuid,
                                                   const uwb::protocol::EventNotification &event,
                                                   const uwb::protocol::LocalPositionEvent &position,
                                                   std::uint64_t hostReceiveTimestampUs) noexcept;

} // namespace uwb::domain
