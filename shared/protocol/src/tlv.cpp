#include "uwb/protocol/tlv.hpp"

#include <utility>

#include "uwb/protocol/wire_reader.hpp"
#include "uwb/protocol/wire_writer.hpp"

namespace uwb::protocol {

Result<std::vector<TlvRecord>> decodeTlvRecords(ConstBytes data) noexcept {
    std::vector<TlvRecord> records;
    WireReader reader{data};

    while (reader.remaining() != 0) {
        if (reader.remaining() < kTlvHeaderSize) {
            return Result<std::vector<TlvRecord>>::error(ProtocolErrorCode::TruncatedPayload,
                                                         static_cast<std::uint32_t>(reader.remaining()));
        }

        TlvRecord record;
        std::uint16_t length = 0;
        if (!reader.readU16(record.type) || !reader.readU16(length)) {
            return Result<std::vector<TlvRecord>>::error(ProtocolErrorCode::TruncatedPayload);
        }
        if (length > reader.remaining()) {
            return Result<std::vector<TlvRecord>>::error(ProtocolErrorCode::TruncatedPayload, length);
        }

        ConstBytes value;
        if (!reader.readSpan(value, length)) {
            return Result<std::vector<TlvRecord>>::error(ProtocolErrorCode::TruncatedPayload);
        }
        record.value.assign(value.begin(), value.end());
        records.push_back(std::move(record));
    }

    return Result<std::vector<TlvRecord>>::ok(std::move(records));
}

ByteBuffer encodeTlvRecords(const std::vector<TlvRecord> &records) noexcept {
    std::size_t total = 0;
    for (const TlvRecord &record : records) {
        total += kTlvHeaderSize + record.value.size();
    }

    ByteBuffer out;
    out.reserve(total);
    WireWriter w{out};
    for (const TlvRecord &record : records) {
        w.writeU16(record.type);
        w.writeU16(static_cast<std::uint16_t>(record.value.size()));
        w.writeBytes(bytesOf(record.value));
    }
    return out;
}

const TlvRecord *findTlvRecord(const std::vector<TlvRecord> &records, std::uint16_t type) noexcept {
    for (const TlvRecord &record : records) {
        if (record.type == type) {
            return &record;
        }
    }
    return nullptr;
}

const char *toString(BackendInfoTlvType type) noexcept {
    switch (type) {
    case BackendInfoTlvType::DecaVersionText: return "DecaVersionText";
    case BackendInfoTlvType::DListText: return "DListText";
    case BackendInfoTlvType::KListText: return "KListText";
    }
    return "UnknownBackendInfoTlv";
}

Result<BackendInfoContainer> decodeBackendInfo(ConstBytes data) noexcept {
    if (data.size() < 2) {
        return Result<BackendInfoContainer>::error(ProtocolErrorCode::TruncatedPayload,
                                                   static_cast<std::uint32_t>(data.size()));
    }

    WireReader reader{data};
    BackendInfoContainer container;
    if (!reader.readU16(container.schemaVersion)) {
        return Result<BackendInfoContainer>::error(ProtocolErrorCode::TruncatedPayload);
    }
    if (container.schemaVersion != kBackendInfoSchemaVersion1) {
        return Result<BackendInfoContainer>::error(ProtocolErrorCode::UnsupportedSchemaVersion,
                                                   container.schemaVersion);
    }

    auto records = decodeTlvRecords(reader.remainingBytes());
    if (!records) {
        return Result<BackendInfoContainer>::error(records.error());
    }

    for (const TlvRecord &record : records.value()) {
        if (!isKnownBackendInfoTlvType(record.type)) {
            return Result<BackendInfoContainer>::error(ProtocolErrorCode::InvalidField, record.type);
        }
    }

    container.records = std::move(records.value());
    return Result<BackendInfoContainer>::ok(std::move(container));
}

ByteBuffer encodeBackendInfo(const BackendInfoContainer &container) noexcept {
    ByteBuffer out;
    out.reserve(2 + container.records.size() * kTlvHeaderSize);
    WireWriter w{out};
    w.writeU16(container.schemaVersion);
    w.writeBytes(encodeTlvRecords(container.records));
    return out;
}

} // namespace uwb::protocol
