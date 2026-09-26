#!/usr/bin/env python3
"""Generate C++ golden vectors for the uwb_protocol unit tests.

Every byte sequence below is derived directly from the wire tables of
uwb_pico_client_server_specification.md using big-endian struct.pack, and the
UUID vector is derived with the Python standard library (uuid.uuid5 / RFC 9562).
The generator is deliberately independent of the C++ implementation so that the
unit tests compare the implementation against the specification rather than
against itself (implementation plan §10.3).

Regenerate with:

    tests/golden/regenerate.sh          # writes the header atomically
    tests/golden/verify.sh              # CI check: committed == regenerated

Do not edit the generated header by hand.
"""

import struct
import uuid

NAMESPACE = uuid.UUID("18659371-d91d-42d0-a58d-13b10305bbee")
BOARD_ID = bytes.fromhex("e661640f7c123456")
DEVICE_UUID = uuid.uuid5(NAMESPACE, BOARD_ID.hex())
CLIENT_UUID = uuid.UUID("11111111-2222-3333-4444-555555555555")

CAP_MASK_ALL = 0x000000000003FFFF
CAP_MASK_DETECTED = 0x000000000001FFFF
DEVICE_NAME = b"pico-anchor"
TCP_PORT = 13401
LOGICAL_ADDRESS_DEVICE = 0x1000
LOGICAL_ADDRESS_CLIENT = 0x0E00

VERSION_BYTE = 0x10
INV_VERSION_BYTE = 0xEF

out = []


def emit(name: str, data: bytes, comment: str) -> None:
    out.append(f"// {comment}")
    out.append(f"inline constexpr std::array<std::uint8_t, {len(data)}> k{name} = {{")
    row = []
    for index, byte in enumerate(data):
        row.append(f"0x{byte:02X}")
        if len(row) == 12 or index == len(data) - 1:
            out.append("    " + ", ".join(row) + ",")
            row = []
    out.append("};")
    out.append("")


def header(payload_type: int, payload_length: int) -> bytes:
    # Specification §13: version, inverse version, payload type, payload length.
    return struct.pack(">BBHI", VERSION_BYTE, INV_VERSION_BYTE, payload_type, payload_length)


def frame(payload_type: int, payload: bytes) -> bytes:
    return header(payload_type, len(payload)) + payload


# ---------------------------------------------------------------------------
# §13 generic header
# ---------------------------------------------------------------------------
emit("HeaderApplicationMessage", header(0x8001, 15),
     "§13 generic header for a 15-byte Application Message")
emit("HeaderEventNotification", header(0x8010, 54),
     "§13 generic header for a 54-byte Event Notification")
emit("HeaderDeviceIdResponse", header(0x0004, 43), "§13 generic header for the Device ID response below")

# ---------------------------------------------------------------------------
# §15 Generic Header NACK
# ---------------------------------------------------------------------------
emit("GenericHeaderNackUnknownType", bytes([0x01]), "§15 NACK payload: unknown payload type")
emit("GenericHeaderNackTooLarge", bytes([0x02]), "§15 NACK payload: message too large")

# ---------------------------------------------------------------------------
# §17 Device Identification Response
# ---------------------------------------------------------------------------
device_id_payload = (
    DEVICE_UUID.bytes
    + struct.pack(">HH", LOGICAL_ADDRESS_DEVICE, TCP_PORT)
    + struct.pack(">QBBB", CAP_MASK_ALL, 0, 1, 4)
    + struct.pack(">B", len(DEVICE_NAME))
    + DEVICE_NAME
)
emit("DeviceIdResponsePayload", device_id_payload, "§17 Device ID response payload")
emit("DeviceIdResponseFrame", frame(0x0004, device_id_payload),
     "§17 Device ID response as a complete frame")

# ---------------------------------------------------------------------------
# §18 Connection activation
# ---------------------------------------------------------------------------
activation_request = struct.pack(">HBB", LOGICAL_ADDRESS_CLIENT, 0x01, 0x00) + CLIENT_UUID.bytes
emit("ActivationRequestPayload", activation_request, "§18.1 activation request (Control role, flags 0)")

activation_response = (
    struct.pack(">HBB", LOGICAL_ADDRESS_DEVICE, 0x01, 0x00)
    + DEVICE_UUID.bytes
    + struct.pack(">IQ", 4096, CAP_MASK_ALL)
)
emit("ActivationResponsePayload", activation_response, "§18.2 activation response (accepted, 4096 max payload)")
emit("ActivationRequestFrame", frame(0x0005, activation_request), "§18.1 activation request frame")
emit("ActivationResponseFrame", frame(0x0006, activation_response), "§18.2 activation response frame")

# ---------------------------------------------------------------------------
# §19 Alive Check
# ---------------------------------------------------------------------------
alive_nonce = struct.pack(">Q", 0x0123456789ABCDEF)
emit("AliveCheckPayload", alive_nonce, "§19 alive check nonce")
emit("AliveCheckRequestFrame", frame(0x0007, alive_nonce), "§19 alive check request frame")
emit("AliveCheckResponseFrame", frame(0x0008, alive_nonce), "§19 alive check response frame")

# ---------------------------------------------------------------------------
# §20 Application Message envelope / ACK / NACK
# ---------------------------------------------------------------------------
read_uuid_pdu = bytes.fromhex("22F000")
envelope = struct.pack(">HHIB3s", LOGICAL_ADDRESS_CLIENT, LOGICAL_ADDRESS_DEVICE, 0x00000042, 0x00, b"\x00\x00\x00")
envelope += read_uuid_pdu
emit("ApplicationEnvelopeReadUuid", envelope, "§20.1 envelope carrying ReadDID 0xF000")
emit("ApplicationEnvelopeAckRequired", struct.pack(">HHIB3s", LOGICAL_ADDRESS_CLIENT, LOGICAL_ADDRESS_DEVICE,
                                                   0x00000042, 0x01, b"\x00\x00\x00") + read_uuid_pdu,
     "§20.1 envelope with ACK_REQUIRED set")

emit("ApplicationAckPayload", struct.pack(">HHI", LOGICAL_ADDRESS_DEVICE, LOGICAL_ADDRESS_CLIENT, 0x00000042),
     "§20.2 positive transport ACK")
emit("ApplicationNackPayload", struct.pack(">HHIB", LOGICAL_ADDRESS_DEVICE, LOGICAL_ADDRESS_CLIENT, 0x00000042, 0x01),
     "§20.3 negative transport ACK (invalid source/target address)")
emit("ApplicationEnvelopeReadUuidFrame", frame(0x8001, envelope), "§20.1 application message frame")

# ---------------------------------------------------------------------------
# §21 negative service response
# ---------------------------------------------------------------------------
emit("NegativeResponseRequestOutOfRange", bytes.fromhex("7F2231"), "§21.4 NRC 0x31 for SID 0x22")
emit("NegativeResponseSession", bytes.fromhex("7F107F"), "§21.4 NRC 0x7F for SID 0x10")

# ---------------------------------------------------------------------------
# §23 Session Control
# ---------------------------------------------------------------------------
emit("SessionControlRequestExtended", bytes.fromhex("1003"), "§23 request Extended Session")
emit("SessionControlRequestDefault", bytes.fromhex("1001"), "§23 request Default Session")
emit("SessionControlResponseExtended", struct.pack(">BBHH", 0x50, 0x03, 1000, 500),
     "§23 response: active session 0x03, P2 1000 ms, P2* 5000 ms")

# ---------------------------------------------------------------------------
# §24 Device Reset
# ---------------------------------------------------------------------------
emit("DeviceResetRequestSoft", bytes.fromhex("1103"), "§24 sub-function 0x03 software reset")
emit("DeviceResetResponseSoft", bytes.fromhex("5103"), "§24 response echo")
emit("DeviceResetRequestHard", bytes.fromhex("1101"), "§24 sub-function 0x01 hard reset")
emit("DeviceResetResponseHard", bytes.fromhex("5101"), "§24 response echo")

# ---------------------------------------------------------------------------
# §25 Security Access
# ---------------------------------------------------------------------------
emit("SecurityRequestSeed", bytes.fromhex("2701"), "§25 request seed")
emit("SecurityResponseSeed", struct.pack(">BB", 0x67, 0x01) + struct.pack(">I", 0xDEADBEEF),
     "§25 seed response (v1 seed is 4 bytes)")
emit("SecuritySendKey", struct.pack(">BBI", 0x27, 0x02, 0xCAFEBABE), "§25 send key")
emit("SecurityResponseKeyOk", bytes.fromhex("6702"), "§25 key accepted")

# ---------------------------------------------------------------------------
# §26 / §27 Read and Write DID
# ---------------------------------------------------------------------------
emit("ReadDidDeviceNameRequest", bytes.fromhex("22F002"), "§26 ReadDID 0xF002")
name_record = struct.pack(">B", len(DEVICE_NAME)) + DEVICE_NAME
emit("ReadDidDeviceNameResponse", struct.pack(">BH", 0x62, 0xF002) + name_record,
     "§26 ReadDID 0xF002 response: DID record per §40.3 (length-prefixed)")
emit("WriteDidDeviceNameRequest", struct.pack(">BH", 0x2E, 0xF002) + name_record,
     "§27 WriteDID 0xF002 request: DID record per §40.3 (length-prefixed)")
emit("WriteDidDeviceNameResponse", struct.pack(">BH", 0x6E, 0xF002), "§27 WriteDID response echo")

# ---------------------------------------------------------------------------
# §28 Routine Control
# ---------------------------------------------------------------------------
emit("RoutineStartMeasurement", struct.pack(">BBH", 0x31, 0x01, 0x0230),
     "§28 start RoutineId 0x0230 without option record")
emit("RoutineStartMeasurementResponse", struct.pack(">BBHB", 0x71, 0x01, 0x0230, 0x01),
     "§28 response: running")
emit("RoutineResultsMeasurement", struct.pack(">BBH", 0x31, 0x03, 0x0230), "§28 request results")
emit("RoutineResultsMeasurementResponse", struct.pack(">BBHB", 0x71, 0x03, 0x0230, 0x03) + struct.pack(">H", 100),
     "§28 response: completed, status record carries a u16 counter")

# ---------------------------------------------------------------------------
# §29 Client Present
# ---------------------------------------------------------------------------
emit("ClientPresentRequest", bytes.fromhex("3E00"), "§29 ClientPresent request")
emit("ClientPresentResponse", bytes.fromhex("7E00"), "§29 ClientPresent response")

# ---------------------------------------------------------------------------
# §30 Execute AT Command (golden AT strings from BU03/BU04 AT manual V1.0.7)
# ---------------------------------------------------------------------------
at_command = b"AT+GETVER"
at_response = b"getver software:V1.0.0\r\nOK\r\n"
emit("ExecuteAtRequestGetVer", struct.pack(">BH", 0x40, len(at_command)) + at_command,
     "§30 ExecuteAt request 'AT+GETVER'")
emit("ExecuteAtResponseGetVer", struct.pack(">BH", 0x80, len(at_response)) + at_response,
     "§30 ExecuteAt response for GETVER (AT manual V1.0.7 example, raw CRLF preserved)")

# ---------------------------------------------------------------------------
# §31 Event Control
# ---------------------------------------------------------------------------
emit("EventSubscribeRequest", struct.pack(">BBHBBH", 0x41, 0x01, 0x0001, 0x01, 0x00, 100),
     "§31.1 subscribe EventId 0x0001 Live, 100 ms")
emit("EventSubscribeResponse", struct.pack(">BBHHBBHH", 0x81, 0x01, 0x0001, 0x0001, 0x01, 0x00, 100, 4),
     "§31.4 subscribe response: streamId 1, accepted 100 ms, queue capacity 4")
emit("EventUnsubscribeRequest", struct.pack(">BBH", 0x41, 0x02, 0x0001), "§31.1 unsubscribe streamId 1")
emit("EventUnsubscribeResponse", struct.pack(">BBH", 0x81, 0x02, 0x0001), "§31.4 unsubscribe response")
emit("EventQueryRequest", struct.pack(">BBH", 0x41, 0x03, 0x0001), "§31.1 query streamId 1")
emit("EventQueryResponse", struct.pack(">BBHHBBHI", 0x81, 0x03, 0x0001, 0x0001, 0x01, 0x01, 100, 3),
     "§31.4 query response: active Live stream, 3 dropped")
emit("EventUnsubscribeAllRequest", bytes.fromhex("4104"), "§31.1 unsubscribe all")
emit("EventUnsubscribeAllResponse", bytes.fromhex("8104"), "§31.4 unsubscribe all response")

# ---------------------------------------------------------------------------
# §32 Event Notification envelope, §34 measurement format v1
# ---------------------------------------------------------------------------
measurement = struct.pack(">HHHH", 0x0001, 0x1001, 0x2001, 0x03FF)
measurement += struct.pack(">ii", 1234, 1200)
measurement += struct.pack(">ii", 45000, 44500)
measurement += struct.pack(">ii", -1000, -950)
measurement += struct.pack(">HH", 850, 0)
emit("MeasurementEventPayload", measurement, "§34 measurement event v1 payload (34 bytes)")

# §32: eventId:u16 | formatVersion:u8 | flags:u8 | streamId:u16 | reserved:u16 | sequence:u32 | timestamp:u64
event_envelope = struct.pack(">HBBHHIQ", 0x0001, 0x01, 0x00, 0x0001, 0x0000, 7, 1712345678901234)
emit("EventEnvelopeHeader", event_envelope, "§32 event envelope fixed part (20 bytes)")
emit("EventNotificationPayload", event_envelope + measurement, "§32 + §34 complete event payload")
emit("EventNotificationFrame", frame(0x8010, event_envelope + measurement),
     "§32 + §34 event notification frame")

# ---------------------------------------------------------------------------
# §35 local position, §36 stream status
# ---------------------------------------------------------------------------
emit("LocalPositionEventPayload", struct.pack(">HH", 0x0001, 0x2001) + struct.pack(">iii", 1000, -2500, 150)
     + struct.pack(">HH", 700, 0x0003), "§35 local position event v1 payload (20 bytes)")
emit("StreamStatusEventPayload", struct.pack(">HBBI", 0x0001, 0x03, 0x01, 5),
     "§36 stream status event: failed after recording queue overflow")

# ---------------------------------------------------------------------------
# §40 DID records
# ---------------------------------------------------------------------------
emit("RecordLogicalAddress", struct.pack(">H", LOGICAL_ADDRESS_DEVICE), "§40.4")
emit("RecordServerFirmwareVersion", struct.pack(">BBHI", 0, 1, 2, 3), "§40.5")
emit("RecordDeviceName", struct.pack(">B", len(DEVICE_NAME)) + DEVICE_NAME, "§40.3")
emit("RecordUwbModuleVersion", struct.pack(">B", 6) + b"V1.0.0", "§40.6")
emit("RecordCapabilityDetection", struct.pack(">QQ", CAP_MASK_ALL, CAP_MASK_DETECTED), "§40.7")
emit("RecordCapabilityOverride", struct.pack(">QQ", 0x0000000000000000, 0x0000000000020000), "§40.8")
emit("RecordEffectiveCapabilities", struct.pack(">Q", CAP_MASK_DETECTED), "§40.9")
emit("RecordTimeoutConfig", struct.pack(">IIIII", 1000, 100, 5000, 60000, 10000), "§40.10 / §52 defaults")
emit("RecordConnectionStatus", struct.pack(">BBBBHH", 1, 2, 4, 0x03, LOGICAL_ADDRESS_CLIENT, 0), "§40.11")
emit("RecordUptime", struct.pack(">Q", 1234567890123), "§40.12")
emit("RecordWifiRssi", struct.pack(">h", -47), "§40.13")
emit("RecordUwbDeviceParameters", struct.pack(">HBBHH", 1, 2, 5, 0, 0), "§40.14")
emit("RecordUwbTwrParameters", struct.pack(">HHHBB", 64, 0, 0x0001, 2, 0)
     + struct.pack(">ffff", 0.1, 1.0, 1.0, 0.0), "§40.15")
emit("RecordUwbPdoaParameters", struct.pack(">HHIHHBB", 0, 0, 1, 1, 0, 1, 0) + struct.pack(">Hii", 0, -45, -100),
     "§40.16")
emit("RecordUwbWorkMode", struct.pack(">B", 1), "§40.17")
emit("RecordUwbMode", struct.pack(">B", 0), "§40.18")
emit("RecordLatestSensorData", struct.pack(">ffff", 0.01, -0.02, 0.98, 12.5), "§40.19")
emit("RecordLatestDistance", struct.pack(">i", 1234), "§40.20")
emit("RecordDeviceCalibration", struct.pack(">Hiiii", 0x001F, 1200, -30, 1500, -200), "§40.23")

# ---------------------------------------------------------------------------
# §40.21 DID 0xF017 backend metadata container
# ---------------------------------------------------------------------------
backend_records = struct.pack(">H", 1)
for tl_type, text in ((1, b"DECA IDW3000"), (2, b"dlist 0x1099"), (3, b"klist 0x1199")):
    backend_records += struct.pack(">HH", tl_type, len(text)) + text
emit("BackendInfoPayload", backend_records, "§40.21 metadata container payload")

# ---------------------------------------------------------------------------
# §40.22 DID 0xF01F complete configuration container
# ---------------------------------------------------------------------------
config_entries = [
    (0xF010, bytes.fromhex("00010205" + "0000" + "0000")),
    (0xF011, struct.pack(">HHHBB", 64, 0, 0x0001, 2, 0) + struct.pack(">ffff", 0.1, 1.0, 1.0, 0.0)),
    (0xF012, struct.pack(">HHIHHBB", 0, 0, 1, 1, 0, 1, 0) + struct.pack(">Hii", 0, -45, -100)),
    (0xF013, struct.pack(">B", 1)),
    (0xF014, struct.pack(">B", 0)),
]
complete = struct.pack(">HH", 1, len(config_entries))
for did_value, data in config_entries:
    complete += struct.pack(">HH", did_value, len(data)) + data
emit("CompleteConfigPayload", complete, "§40.22 complete configuration payload, v1 entry set")

# Duplicate DID: must be rejected (§40.22 step 2).
dup = list(config_entries) + [(0xF013, struct.pack(">B", 0))]
complete_dup = struct.pack(">HH", 1, len(dup))
for did_value, data in dup:
    complete_dup += struct.pack(">HH", did_value, len(data)) + data
emit("CompleteConfigPayloadDuplicate", complete_dup, "§40.22 invalid: duplicate DID 0xF013")

# Wrong record size: 0xF013 carries two bytes instead of one.
bad = [(0xF013, struct.pack(">BB", 1, 0))]
complete_bad = struct.pack(">HH", 1, len(bad))
for did_value, data in bad:
    complete_bad += struct.pack(">HH", did_value, len(data)) + data
emit("CompleteConfigPayloadBadSize", complete_bad, "§40.22 invalid: wrong record length for 0xF013")

# ---------------------------------------------------------------------------
# §6.2 Device UUID derivation (UUIDv5 over the lowercase hex of the board ID)
# ---------------------------------------------------------------------------
emit("BoardId", BOARD_ID, "§6.2 example board ID in exact pico_get_unique_board_id byte order")
emit("DeviceUuid", DEVICE_UUID.bytes,
     f"§6.2 derived Device UUID = UUIDv5(namespace {NAMESPACE}, name {BOARD_ID.hex()!r})")
emit("NamespaceUuid", NAMESPACE.bytes, "§6.2 project namespace UUID")
emit("ClientUuid", CLIENT_UUID.bytes, "§18.1 example client instance UUID")

PRELUDE = """#pragma once

// GENERATED FILE - do not edit by hand.
//
// Regenerate with:
//     tests/golden/regenerate.sh
// Verify with:
//     tests/golden/verify.sh
//
// Every vector is derived from the wire tables of
// uwb_pico_client_server_specification.md with big-endian struct.pack, and the
// UUID vectors come from the Python standard library (uuid.uuid5, RFC 9562).
// The generator never imports or reads the C++ implementation.

#include <array>
#include <cstdint>

namespace uwb::test {
"""

POSTLUDE = "} // namespace uwb::test\n"

import sys

body = "\n".join(out)
sys.stdout.write(PRELUDE + "\n" + body + "\n" + POSTLUDE)
sys.stderr.write(f"generated {len(out)} lines\n")
sys.stderr.write(f"Device UUID : {DEVICE_UUID}\n")
sys.stderr.write(f"Client UUID : {CLIENT_UUID}\n")
