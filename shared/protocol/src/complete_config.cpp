#include "uwb/protocol/complete_config.hpp"

#include <optional>
#include <utility>

#include "uwb/protocol/constants.hpp"
#include "uwb/protocol/did_records.hpp"
#include "uwb/protocol/wire_reader.hpp"
#include "uwb/protocol/wire_writer.hpp"

namespace uwb::protocol {

Result<CompleteUwbConfig> decodeCompleteUwbConfig(ConstBytes data) noexcept {
    if (data.size() < kCompleteConfigHeaderSize) {
        return Result<CompleteUwbConfig>::error(ProtocolErrorCode::TruncatedPayload,
                                                static_cast<std::uint32_t>(data.size()));
    }

    WireReader reader{data};
    CompleteUwbConfig config;
    std::uint16_t entryCount = 0;
    if (!reader.readU16(config.schemaVersion) || !reader.readU16(entryCount)) {
        return Result<CompleteUwbConfig>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (config.schemaVersion != kCompleteConfigSchemaVersion1) {
        return Result<CompleteUwbConfig>::error(ProtocolErrorCode::UnsupportedSchemaVersion, config.schemaVersion);
    }

    for (std::uint16_t i = 0; i < entryCount; ++i) {
        if (reader.remaining() < 4) {
            return Result<CompleteUwbConfig>::error(ProtocolErrorCode::TruncatedPayload,
                                                    static_cast<std::uint32_t>(reader.remaining()));
        }

        DidRecordEntry entry;
        std::uint16_t length = 0;
        if (!reader.readU16(entry.did) || !reader.readU16(length)) {
            return Result<CompleteUwbConfig>::error(ProtocolErrorCode::TruncatedPayload);
        }
        if (length > reader.remaining()) {
            return Result<CompleteUwbConfig>::error(ProtocolErrorCode::TruncatedPayload, length);
        }

        ConstBytes payload;
        if (!reader.readSpan(payload, length)) {
            return Result<CompleteUwbConfig>::error(ProtocolErrorCode::TruncatedPayload);
        }
        entry.data.assign(payload.begin(), payload.end());
        config.entries.push_back(std::move(entry));
    }

    if (reader.remaining() != 0) {
        // Trailing bytes after entryCount entries: inconsistent length.
        return Result<CompleteUwbConfig>::error(ProtocolErrorCode::UnexpectedPayloadLength,
                                                static_cast<std::uint32_t>(reader.remaining()));
    }

    auto valid = validateCompleteUwbConfig(config);
    if (!valid) {
        return Result<CompleteUwbConfig>::error(valid.error());
    }
    return Result<CompleteUwbConfig>::ok(std::move(config));
}

ByteBuffer encodeCompleteUwbConfig(const CompleteUwbConfig &config) noexcept {
    std::size_t total = kCompleteConfigHeaderSize;
    for (const DidRecordEntry &entry : config.entries) {
        total += 4 + entry.data.size();
    }

    ByteBuffer out;
    out.reserve(total);
    WireWriter w{out};
    w.writeU16(config.schemaVersion);
    w.writeU16(static_cast<std::uint16_t>(config.entries.size()));
    for (const DidRecordEntry &entry : config.entries) {
        w.writeU16(entry.did);
        w.writeU16(static_cast<std::uint16_t>(entry.data.size()));
        w.writeBytes(bytesOf(entry.data));
    }
    return out;
}

Result<bool> validateCompleteUwbConfig(const CompleteUwbConfig &config) noexcept {
    if (config.schemaVersion != kCompleteConfigSchemaVersion1) {
        return Result<bool>::error(ProtocolErrorCode::UnsupportedSchemaVersion, config.schemaVersion);
    }

    for (std::size_t i = 0; i < config.entries.size(); ++i) {
        const DidRecordEntry &entry = config.entries[i];

        if (!isKnownDid(entry.did)) {
            return Result<bool>::error(ProtocolErrorCode::InvalidField, entry.did);
        }
        if (!isCompleteConfigDid(entry.did)) {
            return Result<bool>::error(ProtocolErrorCode::InvalidField, entry.did);
        }
        if (entry.data.size() > kMaxProtocolPayload) {
            return Result<bool>::error(ProtocolErrorCode::PayloadTooLarge,
                                       static_cast<std::uint32_t>(entry.data.size()));
        }

        for (std::size_t j = 0; j < i; ++j) {
            if (config.entries[j].did == entry.did) {
                return Result<bool>::error(ProtocolErrorCode::DuplicateEntry, entry.did);
            }
        }

        const std::optional<std::size_t> expected = expectedDidRecordSize(entry.didId());
        if (expected && *expected != entry.data.size()) {
            return Result<bool>::error(ProtocolErrorCode::UnexpectedPayloadLength,
                                       static_cast<std::uint32_t>(entry.data.size()));
        }

        // Reject records that are structurally invalid even at the right size.
        switch (entry.didId()) {
        case Did::UwbDeviceParameters:
            if (!decodeUwbDeviceParametersRecord(entry.dataBytes())) {
                return Result<bool>::error(ProtocolErrorCode::InvalidField, entry.did);
            }
            break;
        case Did::UwbTwrParameters:
            if (!decodeUwbTwrParametersRecord(entry.dataBytes())) {
                return Result<bool>::error(ProtocolErrorCode::InvalidField, entry.did);
            }
            break;
        case Did::UwbPdoaParameters:
            if (!decodeUwbPdoaParametersRecord(entry.dataBytes())) {
                return Result<bool>::error(ProtocolErrorCode::InvalidField, entry.did);
            }
            break;
        default:
            break;
        }
    }

    return Result<bool>::ok(true);
}

const DidRecordEntry *findCompleteConfigEntry(const CompleteUwbConfig &config, Did did) noexcept {
    for (const DidRecordEntry &entry : config.entries) {
        if (entry.did == static_cast<std::uint16_t>(did)) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace uwb::protocol
