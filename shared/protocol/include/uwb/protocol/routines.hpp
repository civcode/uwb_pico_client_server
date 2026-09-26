#pragma once

#include <cstdint>

namespace uwb::protocol {

// Specification §42.
enum class RoutineId : std::uint16_t {
    InitializeUwbBackend = 0x0200,
    RestartUwbModule = 0x0201,
    RestoreUwbDefaults = 0x0202,
    SaveUwbConfiguration = 0x0203,
    AddTag = 0x0210,
    DeleteTag = 0x0211,
    UwbMeasurementAcquisition = 0x0230,
    DeviceCalibration = 0x0240,
    TestLed = 0x02F0,
    TestOled = 0x02F1,
};

[[nodiscard]] constexpr bool isKnownRoutine(std::uint16_t value) noexcept {
    switch (value) {
    case 0x0200: case 0x0201: case 0x0202: case 0x0203: case 0x0210:
    case 0x0211: case 0x0230: case 0x0240: case 0x02F0: case 0x02F1:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] constexpr bool isKnownRoutine(RoutineId id) noexcept {
    return isKnownRoutine(static_cast<std::uint16_t>(id));
}

// Routines that are cancellable via stopRoutine (specification §42, §28).
[[nodiscard]] constexpr bool routineIsCancellable(RoutineId id) noexcept {
    switch (id) {
    case RoutineId::InitializeUwbBackend:
    case RoutineId::UwbMeasurementAcquisition:
    case RoutineId::DeviceCalibration:
    case RoutineId::TestLed:
    case RoutineId::TestOled:
        return true;
    case RoutineId::RestartUwbModule:
    case RoutineId::RestoreUwbDefaults:
    case RoutineId::SaveUwbConfiguration:
    case RoutineId::AddTag:
    case RoutineId::DeleteTag:
        return false;
    }
    return false;
}

[[nodiscard]] const char *routineName(RoutineId id) noexcept;

// RoutineControl sub-functions (specification §28).
enum class RoutineControlType : std::uint8_t {
    Start = 0x01,
    Stop = 0x02,
    RequestResults = 0x03,
};

enum class RoutineState : std::uint8_t {
    Idle = 0x00,
    Running = 0x01,
    Stopping = 0x02,
    Completed = 0x03,
    Failed = 0x04,
    Cancelled = 0x05,
};

[[nodiscard]] constexpr bool isKnownRoutineState(std::uint8_t value) noexcept {
    return value <= 0x05;
}

[[nodiscard]] constexpr bool isKnownRoutineControlType(std::uint8_t value) noexcept {
    return value >= 0x01 && value <= 0x03;
}

} // namespace uwb::protocol
