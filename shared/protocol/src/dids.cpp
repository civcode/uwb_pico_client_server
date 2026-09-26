#include "uwb/protocol/dids.hpp"

namespace uwb::protocol {

DidAccess didAccess(Did did) noexcept {
    switch (did) {
    case Did::DeviceName:
    case Did::LogicalAddress:
    case Did::CapabilityOverride:
    case Did::TimeoutConfiguration:
    case Did::UwbDeviceParameters:
    case Did::UwbTwrParameters:
    case Did::UwbPdoaParameters:
    case Did::UwbWorkMode:
    case Did::UwbMode:
    case Did::UwbCompleteConfiguration:
    case Did::DeviceCalibration:
        return DidAccess::ReadWrite;
    case Did::DeviceUuid:
    case Did::PicoBoardUniqueId:
    case Did::PicoFirmwareVersion:
    case Did::UwbModuleVersion:
    case Did::CapabilityDetection:
    case Did::EffectiveCapabilities:
    case Did::ConnectionStatus:
    case Did::Uptime:
    case Did::WifiRssi:
    case Did::UwbLatestSensorData:
    case Did::UwbLatestDistance:
    case Did::UwbMiscellaneousMetadata:
        return DidAccess::ReadOnly;
    }
    return DidAccess::ReadOnly;
}

DidPersistence didPersistence(Did did) noexcept {
    switch (did) {
    case Did::DeviceUuid:
    case Did::PicoBoardUniqueId:
    case Did::CapabilityDetection:
    case Did::EffectiveCapabilities:
    case Did::ConnectionStatus:
    case Did::Uptime:
    case Did::WifiRssi:
    case Did::UwbLatestSensorData:
    case Did::UwbLatestDistance:
    case Did::UwbMiscellaneousMetadata:
        return DidPersistence::Runtime;
    case Did::PicoFirmwareVersion:
        return DidPersistence::Firmware;
    case Did::DeviceName:
    case Did::LogicalAddress:
    case Did::CapabilityOverride:
    case Did::TimeoutConfiguration:
        return DidPersistence::PicoPersistent;
    case Did::UwbDeviceParameters:
    case Did::UwbTwrParameters:
    case Did::UwbPdoaParameters:
    case Did::UwbCompleteConfiguration:
        return DidPersistence::UwbRuntime;
    case Did::UwbWorkMode:
    case Did::UwbMode:
        return DidPersistence::UwbConditional;
    case Did::UwbModuleVersion:
        return DidPersistence::Runtime;
    case Did::DeviceCalibration:
        return DidPersistence::UwbOrPico;
    }
    return DidPersistence::Runtime;
}

const char *didName(Did did) noexcept {
    switch (did) {
    case Did::DeviceUuid: return "DeviceUuid";
    case Did::PicoBoardUniqueId: return "PicoBoardUniqueId";
    case Did::DeviceName: return "DeviceName";
    case Did::LogicalAddress: return "LogicalAddress";
    case Did::PicoFirmwareVersion: return "PicoFirmwareVersion";
    case Did::UwbModuleVersion: return "UwbModuleVersion";
    case Did::CapabilityDetection: return "CapabilityDetection";
    case Did::CapabilityOverride: return "CapabilityOverride";
    case Did::EffectiveCapabilities: return "EffectiveCapabilities";
    case Did::TimeoutConfiguration: return "TimeoutConfiguration";
    case Did::ConnectionStatus: return "ConnectionStatus";
    case Did::Uptime: return "Uptime";
    case Did::WifiRssi: return "WifiRssi";
    case Did::UwbDeviceParameters: return "UwbDeviceParameters";
    case Did::UwbTwrParameters: return "UwbTwrParameters";
    case Did::UwbPdoaParameters: return "UwbPdoaParameters";
    case Did::UwbWorkMode: return "UwbWorkMode";
    case Did::UwbMode: return "UwbMode";
    case Did::UwbLatestSensorData: return "UwbLatestSensorData";
    case Did::UwbLatestDistance: return "UwbLatestDistance";
    case Did::UwbMiscellaneousMetadata: return "UwbMiscellaneousMetadata";
    case Did::UwbCompleteConfiguration: return "UwbCompleteConfiguration";
    case Did::DeviceCalibration: return "DeviceCalibration";
    }
    return "UnknownDid";
}

std::optional<std::size_t> didRecordSize(Did did) noexcept {
    switch (did) {
    case Did::DeviceUuid:                 return std::size_t{16};  // 40.1
    case Did::PicoBoardUniqueId:          return std::size_t{8};   // 40.2
    case Did::DeviceName:                 return std::nullopt;     // 40.3 (length-prefixed text)
    case Did::LogicalAddress:             return std::size_t{2};   // 40.4
    case Did::PicoFirmwareVersion:        return std::size_t{8};   // 40.5
    case Did::UwbModuleVersion:           return std::nullopt;     // 40.6 (length-prefixed text)
    case Did::CapabilityDetection:        return std::size_t{16};  // 40.7
    case Did::CapabilityOverride:         return std::size_t{16};  // 40.8
    case Did::EffectiveCapabilities:      return std::size_t{8};   // 40.9
    case Did::TimeoutConfiguration:       return std::size_t{20};  // 40.10
    case Did::ConnectionStatus:           return std::size_t{8};   // 40.11
    case Did::Uptime:                     return std::size_t{8};   // 40.12
    case Did::WifiRssi:                   return std::size_t{2};   // 40.13
    case Did::UwbDeviceParameters:        return std::size_t{8};   // 40.14
    case Did::UwbTwrParameters:           return std::size_t{24};  // 40.15
    case Did::UwbPdoaParameters:          return std::size_t{24};  // 40.16
    case Did::UwbWorkMode:                return std::size_t{1};   // 40.17
    case Did::UwbMode:                    return std::size_t{1};   // 40.18
    case Did::UwbLatestSensorData:        return std::size_t{16};  // 40.19
    case Did::UwbLatestDistance:          return std::size_t{4};   // 40.20
    case Did::UwbMiscellaneousMetadata:   return std::nullopt;     // 40.21 (TLV container)
    case Did::UwbCompleteConfiguration:   return std::nullopt;     // 40.22 (TLV container)
    case Did::DeviceCalibration:          return std::size_t{18};  // 40.23
    }
    return std::nullopt;
}

} // namespace uwb::protocol
