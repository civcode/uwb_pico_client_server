#include "uwb/protocol/capabilities.hpp"

namespace uwb::protocol {

const char *capabilityName(Capability capability) noexcept {
    switch (capability) {
    case Capability::Range: return "CAP_RANGE";
    case Capability::PdoaAzimuth: return "CAP_PDOA_AZIMUTH";
    case Capability::PdoaElevation: return "CAP_PDOA_ELEVATION";
    case Capability::UwbLocalPosition: return "CAP_UWB_LOCAL_POSITION";
    case Capability::SensorData: return "CAP_SENSOR_DATA";
    case Capability::UwbConfigRead: return "CAP_UWB_CONFIG_READ";
    case Capability::UwbConfigWrite: return "CAP_UWB_CONFIG_WRITE";
    case Capability::TagManagement: return "CAP_TAG_MANAGEMENT";
    case Capability::RawAt: return "CAP_RAW_AT";
    case Capability::DeviceCalibration: return "CAP_DEVICE_CALIBRATION";
    case Capability::StreamLive: return "CAP_STREAM_LIVE";
    case Capability::StreamRecording: return "CAP_STREAM_RECORDING";
    case Capability::MultiObserver: return "CAP_MULTI_OBSERVER";
    case Capability::SecurityAccess: return "CAP_SECURITY_ACCESS";
    case Capability::PersistentPicoConfig: return "CAP_PERSISTENT_PICO_CONFIG";
    case Capability::CompleteConfigWrite: return "CAP_COMPLETE_CONFIG_WRITE";
    case Capability::TimeSync: return "CAP_TIME_SYNC";
    case Capability::OtaUpdate: return "CAP_OTA_UPDATE";
    }
    return "CAP_UNKNOWN";
}

} // namespace uwb::protocol
