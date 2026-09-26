#include "uwb/protocol/routines.hpp"

namespace uwb::protocol {

const char *routineName(RoutineId id) noexcept {
    switch (id) {
    case RoutineId::RestartUwbModule: return "RestartUwbModule";
    case RoutineId::RestoreUwbDefaults: return "RestoreUwbDefaults";
    case RoutineId::SaveUwbConfiguration: return "SaveUwbConfiguration";
    case RoutineId::AddTag: return "AddTag";
    case RoutineId::DeleteTag: return "DeleteTag";
    case RoutineId::InitializeUwbBackend: return "InitializeUwbBackend";
    case RoutineId::UwbMeasurementAcquisition: return "UwbMeasurementAcquisition";
    case RoutineId::DeviceCalibration: return "DeviceCalibration";
    case RoutineId::TestLed: return "TestLed";
    case RoutineId::TestOled: return "TestOled";
    }
    return "UnknownRoutine";
}

} // namespace uwb::protocol
