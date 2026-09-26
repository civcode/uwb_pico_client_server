#include <cstdint>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "uwb/protocol/complete_config.hpp"
#include "uwb/protocol/did_records.hpp"
#include "uwb/protocol/dids.hpp"
#include "uwb/protocol/tlv.hpp"

#include "gen/golden_vectors.hpp"
#include "test_helpers.hpp"

using namespace uwb::protocol;
using uwb::test::sameBytes;
using uwb::test::view;

namespace {

std::vector<DidRecordEntry> goldenEntries() {
    std::vector<DidRecordEntry> entries;
    entries.push_back(DidRecordEntry{0xF010, ByteBuffer{uwb::test::kRecordUwbDeviceParameters.begin(),
                                                       uwb::test::kRecordUwbDeviceParameters.end()}});
    entries.push_back(DidRecordEntry{0xF011, ByteBuffer{uwb::test::kRecordUwbTwrParameters.begin(),
                                                        uwb::test::kRecordUwbTwrParameters.end()}});
    entries.push_back(DidRecordEntry{0xF012, ByteBuffer{uwb::test::kRecordUwbPdoaParameters.begin(),
                                                        uwb::test::kRecordUwbPdoaParameters.end()}});
    entries.push_back(DidRecordEntry{0xF013, ByteBuffer{uwb::test::kRecordUwbWorkMode.begin(),
                                                        uwb::test::kRecordUwbWorkMode.end()}});
    entries.push_back(DidRecordEntry{0xF014, ByteBuffer{uwb::test::kRecordUwbMode.begin(),
                                                        uwb::test::kRecordUwbMode.end()}});
    return entries;
}

} // namespace

TEST_CASE("TLV records golden (§40.21)", "[protocol][tlv][golden]") {
    auto records = decodeTlvRecords(ConstBytes{uwb::test::kBackendInfoPayload.data() + 2,
                                               uwb::test::kBackendInfoPayload.size() - 2});
    REQUIRE(records.ok());
    REQUIRE(records->size() == 3);
    CHECK((*records)[0].type == 0x0001);
    CHECK(std::string((*records)[0].value.begin(), (*records)[0].value.end()) == "DECA IDW3000");
    CHECK((*records)[1].type == 0x0002);
    CHECK(std::string((*records)[1].value.begin(), (*records)[1].value.end()) == "dlist 0x1099");
    CHECK((*records)[2].type == 0x0003);
    CHECK(std::string((*records)[2].value.begin(), (*records)[2].value.end()) == "klist 0x1199");

    CHECK(sameBytes(bytesOf(encodeTlvRecords(records.value())),
                          ConstBytes{uwb::test::kBackendInfoPayload.data() + 2, uwb::test::kBackendInfoPayload.size() - 2}));
}

TEST_CASE("Backend info container golden (§40.21)", "[protocol][tlv][golden]") {
    auto container = decodeBackendInfo(view(uwb::test::kBackendInfoPayload));
    REQUIRE(container.ok());
    CHECK(container->schemaVersion == kBackendInfoSchemaVersion1);
    REQUIRE(container->records.size() == 3);

    const TlvRecord *dlist = findTlvRecord(container->records, static_cast<std::uint16_t>(BackendInfoTlvType::DListText));
    REQUIRE(dlist != nullptr);
    CHECK(std::string(dlist->value.begin(), dlist->value.end()) == "dlist 0x1099");
    CHECK(findTlvRecord(container->records, 0x00FF) == nullptr);

    CHECK(sameBytes(bytesOf(encodeBackendInfo(container.value())), view(uwb::test::kBackendInfoPayload)));

    // Unknown schema version is rejected.
    ByteBuffer v2 = encodeBackendInfo(container.value());
    v2[0] = 0x00;
    v2[1] = 0x02;
    auto rejected = decodeBackendInfo(bytesOf(v2));
    REQUIRE(rejected.failed());
    CHECK(rejected.code() == ProtocolErrorCode::UnsupportedSchemaVersion);

    // A record that claims more bytes than are present is rejected.
    const std::array<std::uint8_t, 5> truncated = {0x00, 0x01, 0x00, 0x0A, 0x41};
    CHECK(decodeBackendInfo(view(truncated)).failed());
}

TEST_CASE("Complete UWB configuration golden (§40.22)", "[protocol][container][golden]") {
    auto config = decodeCompleteUwbConfig(view(uwb::test::kCompleteConfigPayload));
    REQUIRE(config.ok());
    CHECK(config->schemaVersion == 1);
    REQUIRE(config->entries.size() == 5);

    // §40.22: version 1 contains 0xF010..0xF014.
    CHECK(config->entries[0].did == 0xF010);
    CHECK(config->entries[1].did == 0xF011);
    CHECK(config->entries[2].did == 0xF012);
    CHECK(config->entries[3].did == 0xF013);
    CHECK(config->entries[4].did == 0xF014);
    CHECK(config->entries[0].data.size() == 8);
    CHECK(config->entries[1].data.size() == 24);
    CHECK(config->entries[2].data.size() == 24);
    CHECK(config->entries[3].data.size() == 1);
    CHECK(config->entries[4].data.size() == 1);

    auto device = decodeUwbDeviceParametersRecord(config->entries[0].dataBytes());
    REQUIRE(device.ok());
    CHECK(device->channel == 5);

    CHECK(sameBytes(bytesOf(encodeCompleteUwbConfig(config.value())), view(uwb::test::kCompleteConfigPayload)));
}

TEST_CASE("Complete UWB configuration rebuilds from typed records", "[protocol][container]") {
    CompleteUwbConfig config;
    config.entries = goldenEntries();
    CHECK(validateCompleteUwbConfig(config).ok());

    ByteBuffer bytes = encodeCompleteUwbConfig(config);
    CHECK(sameBytes(bytesOf(bytes), view(uwb::test::kCompleteConfigPayload)));

    auto decoded = decodeCompleteUwbConfig(bytesOf(bytes));
    REQUIRE(decoded.ok());
    CHECK(decoded->entries.size() == config.entries.size());

    const DidRecordEntry *workMode = findCompleteConfigEntry(decoded.value(), Did::UwbWorkMode);
    REQUIRE(workMode != nullptr);
    CHECK(workMode->did == 0xF013);
    CHECK(findCompleteConfigEntry(decoded.value(), Did::DeviceName) == nullptr);
}

TEST_CASE("Complete UWB configuration rejects duplicate DIDs (§40.22 step 2)", "[protocol][container][golden]") {
    auto rejected = decodeCompleteUwbConfig(view(uwb::test::kCompleteConfigPayloadDuplicate));
    REQUIRE(rejected.failed());
    CHECK(rejected.code() == ProtocolErrorCode::DuplicateEntry);
    CHECK(rejected.error().detail == 0xF013U);
}

TEST_CASE("Complete UWB configuration rejects wrong record size (§40.22 step 3)", "[protocol][container][golden]") {
    auto rejected = decodeCompleteUwbConfig(view(uwb::test::kCompleteConfigPayloadBadSize));
    REQUIRE(rejected.failed());
    CHECK(rejected.code() == ProtocolErrorCode::UnexpectedPayloadLength);
}

TEST_CASE("Complete UWB configuration rejects DIDs outside the v1 set (§40.22)", "[protocol][container]") {
    CompleteUwbConfig config;
    config.entries = {DidRecordEntry{0xF003, ByteBuffer{0x10, 0x00}}};
    auto rejected = validateCompleteUwbConfig(config);
    REQUIRE(rejected.failed());
    CHECK(rejected.code() == ProtocolErrorCode::InvalidField);

    CompleteUwbConfig unknown;
    unknown.entries = {DidRecordEntry{0xF0EE, ByteBuffer{0x00}}};
    CHECK(validateCompleteUwbConfig(unknown).failed());
}

TEST_CASE("Complete UWB configuration rejects structurally invalid records", "[protocol][container]") {
    // Right size, invalid flag bits inside 0xF011.
    UwbTwrParametersRecord bad;
    bad.flags = static_cast<std::uint16_t>(kTwrKnownFlags | 0x8000);
    CompleteUwbConfig config;
    config.entries = {DidRecordEntry{0xF011, encodeRecord(bad)}};
    auto rejected = validateCompleteUwbConfig(config);
    REQUIRE(rejected.failed());
    CHECK(rejected.code() == ProtocolErrorCode::InvalidField);
}

TEST_CASE("Complete UWB configuration framing errors", "[protocol][container]") {
    CHECK(decodeCompleteUwbConfig(ConstBytes{}).failed());
    const std::array<std::uint8_t, 3> truncated = {0x00, 0x01, 0x00};
    CHECK(decodeCompleteUwbConfig(view(truncated)).failed());

    // entryCount promises more entries than are present.
    const std::array<std::uint8_t, 6> header = {0x00, 0x01, 0x00, 0x05, 0x00, 0x00};
    CHECK(decodeCompleteUwbConfig(view(header)).failed());

    // Trailing bytes after the declared entry count.
    ByteBuffer trailing{uwb::test::kCompleteConfigPayload.begin(), uwb::test::kCompleteConfigPayload.end()};
    trailing.push_back(0x00);
    CHECK(decodeCompleteUwbConfig(bytesOf(trailing)).failed());

    // Unsupported schema version.
    ByteBuffer v2{uwb::test::kCompleteConfigPayload.begin(), uwb::test::kCompleteConfigPayload.end()};
    v2[1] = 0x02;
    auto rejected = decodeCompleteUwbConfig(bytesOf(v2));
    REQUIRE(rejected.failed());
    CHECK(rejected.code() == ProtocolErrorCode::UnsupportedSchemaVersion);
}

TEST_CASE("Empty complete configuration is well formed", "[protocol][container]") {
    const std::array<std::uint8_t, 4> empty = {0x00, 0x01, 0x00, 0x00};
    auto config = decodeCompleteUwbConfig(view(empty));
    REQUIRE(config.ok());
    CHECK(config->entries.empty());
    CHECK(validateCompleteUwbConfig(config.value()).ok());
}
