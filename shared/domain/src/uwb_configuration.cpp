#include "uwb/domain/uwb_configuration.hpp"

#include <algorithm>
#include <functional>

namespace uwb::domain {

namespace {

using uwb::protocol::Did;
using uwb::protocol::DidRecordEntry;

template <class Record>
[[nodiscard]] std::optional<Record> decodeEntry(const UwbConfiguration &config, Did did,
                                                const std::function<std::optional<Record>(uwb::protocol::ConstBytes)>
                                                    &decode) noexcept {
    const DidRecordEntry *entry = uwb::protocol::findCompleteConfigEntry(config, did);
    if (entry == nullptr) {
        return std::nullopt;
    }
    return decode(entry->dataBytes());
}

} // namespace

std::optional<uwb::protocol::UwbDeviceParametersRecord> findDeviceParameters(const UwbConfiguration &config) noexcept {
    return decodeEntry<uwb::protocol::UwbDeviceParametersRecord>(
        config, Did::UwbDeviceParameters, [](uwb::protocol::ConstBytes data) -> std::optional<
            uwb::protocol::UwbDeviceParametersRecord> {
            auto decoded = uwb::protocol::decodeUwbDeviceParametersRecord(data);
            if (!decoded.ok()) {
                return std::nullopt;
            }
            return decoded.value();
        });
}

std::optional<uwb::protocol::UwbTwrParametersRecord> findTwrParameters(const UwbConfiguration &config) noexcept {
    return decodeEntry<uwb::protocol::UwbTwrParametersRecord>(
        config, Did::UwbTwrParameters, [](uwb::protocol::ConstBytes data) -> std::optional<
            uwb::protocol::UwbTwrParametersRecord> {
            auto decoded = uwb::protocol::decodeUwbTwrParametersRecord(data);
            if (!decoded.ok()) {
                return std::nullopt;
            }
            return decoded.value();
        });
}

std::optional<uwb::protocol::UwbPdoaParametersRecord> findPdoaParameters(const UwbConfiguration &config) noexcept {
    return decodeEntry<uwb::protocol::UwbPdoaParametersRecord>(
        config, Did::UwbPdoaParameters, [](uwb::protocol::ConstBytes data) -> std::optional<
            uwb::protocol::UwbPdoaParametersRecord> {
            auto decoded = uwb::protocol::decodeUwbPdoaParametersRecord(data);
            if (!decoded.ok()) {
                return std::nullopt;
            }
            return decoded.value();
        });
}

std::optional<std::uint8_t> findWorkMode(const UwbConfiguration &config) noexcept {
    return decodeEntry<std::uint8_t>(config, Did::UwbWorkMode, [](uwb::protocol::ConstBytes data) -> std::optional<
        std::uint8_t> {
        auto decoded = uwb::protocol::decodeU8Record(data);
        if (!decoded.ok()) {
            return std::nullopt;
        }
        return decoded.value();
    });
}

std::optional<std::uint8_t> findMode(const UwbConfiguration &config) noexcept {
    return decodeEntry<std::uint8_t>(config, Did::UwbMode, [](uwb::protocol::ConstBytes data) -> std::optional<
        std::uint8_t> {
        auto decoded = uwb::protocol::decodeU8Record(data);
        if (!decoded.ok()) {
            return std::nullopt;
        }
        return decoded.value();
    });
}

bool upsertConfigurationEntry(UwbConfiguration &config, std::uint16_t did, uwb::protocol::ByteBuffer data) noexcept {
    if (!uwb::protocol::isCompleteConfigDid(did)) {
        return false;
    }
    for (DidRecordEntry &entry : config.entries) {
        if (entry.did == did) {
            entry.data = std::move(data);
            return true;
        }
    }
    DidRecordEntry entry;
    entry.did = did;
    entry.data = std::move(data);
    config.entries.push_back(std::move(entry));
    std::sort(config.entries.begin(), config.entries.end(),
              [](const DidRecordEntry &a, const DidRecordEntry &b) { return a.did < b.did; });
    return true;
}

} // namespace uwb::domain
