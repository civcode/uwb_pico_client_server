# Protocol decisions and specification reconciliation

Records the decisions taken while implementing the shared protocol library
(Phase 1) and how each relates to
[`../uwb_pico_client_server_specification.md`](../uwb_pico_client_server_specification.md)
and
[`../uwb_pico_client_server_implementation_plan.md`](../uwb_pico_client_server_implementation_plan.md).

Precedence (implementation plan §81.3): **specification → plan → golden vectors →
implementation convenience**. Where the specification leaves something open, the
decision is stated here instead of being implicit in code.

---

## 1. Endianness, framing, and error severity

* All multi-byte integers are **big-endian** (specification §12). No C++ struct is
  `memcpy`'d onto the wire: encoders use `WireWriter`, decoders use `WireReader`,
  and both bounds-check every field.
* Reserved fields are transmitted as zero and **rejected when non-zero**
  (`ProtocolErrorCode::InvalidReservedBits`), matching the "Reserved, zero"
  wording of §17, §32, §34, and §40.
* Header validation order is the normative order of specification §13
  (8 header bytes → inverse byte → supported major version → payload type →
  payload length ≤ maximum → complete payload → dispatch one frame), asserted by
  `generic_header_test.cpp:"Header validation order is normative (§13)"`.
* Severity split:
  * **Non-severe** — an unlisted payload type (§14 "values not listed are
    reserved") is kept as a frame with the raw `PayloadType` value, so the server
    can answer with Generic Header NACK `UnknownPayloadType` (`0x01`) and keep the
    connection open. Callers classify it with `isKnownPayloadType()`.
  * **Severe** — bad version/inverse pair, unsupported major version, or a payload
    length above the parser maximum latch `FrameParser` into `failed()`
    (`FrameParser::failed()`, cleared only by `reset()`), matching "SHOULD send a
    NACK when possible and then close the TCP connection" (§15).
* `FrameParser` validates the length field **before** buffering the payload, and
  its `maxPayload` can be lowered to a connection's `MAX_PROTOCOL_PAYLOAD`
  negotiated during activation.
* Minor protocol versions are tolerated by `ProtocolVersion` (§14); a payload
  layout change is rejected by that payload's decoder
  (`ProtocolErrorCode::UnsupportedSchemaVersion`), not by the header check.

## 2. Golden vectors

`tests/golden/gen_golden_vectors.py` writes `tests/unit/gen/golden_vectors.hpp`
(85 vectors) with `struct.pack` on the specification tables; UUID vectors come
from `uuid.uuid5` (RFC 9562). The generator never imports or reads the C++
implementation (plan §10.3).

```bash
tests/golden/regenerate.sh   # atomic regeneration + minimum-vector guard
tests/golden/verify.sh       # CI: committed header == fresh regeneration
```

| Vector group | Specification source |
|---|---|
| Generic headers, NACK | §13, §15 |
| Device ID, activation, alive check, application envelope/ACK | §16–§20 |
| Session control, device reset, security, DID read/write, routines, client present, AT | §23–§30, §21.3 NRC |
| Event control, event envelope, event payloads | §31–§36 |
| DID records, TLV containers, complete configuration | §40 |
| Device UUID `5640b881-6c2f-5b43-a3d8-b62d4845671f` | §18/§40.1/§40.2 — UUIDv5(`18659371-d91d-42d0-a58d-13b10305bbee`, 16 lowercase hex of board ID `e6 61 64 0f 7c 12 34 56`) |
| `AT+GETVER` → `getver software:V1.0.0\r\nOK\r\n` | AT manual **V1.0.7** (§30 golden source) |

## 3. Concrete wire sizes adopted for v1

These are the sizes the codecs enforce; each is the sum of the specification's
field table and is pinned by a golden vector.

| Item | Size | Layout |
|---|---:|---|
| Generic header | 8 | `version:u8 inverse:u8 type:u16 length:u32` |
| Device ID request | 0 | §16 (non-zero payload rejected) |
| Device ID response | 32 + name length (**43** with an 11-byte name) | §17 |
| Activation request | 20 | `logical:u16 role:u8 flags:u8 uuid:16` (§18.1) |
| Activation response | 32 | §18.2 |
| Alive check request/response | 8 each | nonce echoed (§19) |
| Session control request | 2 | `0x10 session:u8` |
| Session control response | 6 | `0x50 session:u8 p2Ms:u16 p2Star10ms:u16` (§23) |
| Security request seed | 2 | `0x27 0x01` |
| Security send key | 6 | `0x27 0x02 key:u32` |
| Security seed response | 6 | `0x67 0x01 seed:u32` |
| Security key response | 2 | `0x67 0x02` (failure via NRC) |
| Read DID request / response | 3 / 3 + data | §26 |
| Write DID request / response | 3 + data / 3 | §27 |
| Routine control request / response | §28 tables | `0x31`, `0x71` |
| Client present | 2 / 2 | `0x3E 0x00`, `0x7E 0x00` (§29) |
| AT request / response | 3 + cmd (≤ 512 cmd) / 3 + raw | §30 |
| Event subscribe req / resp | 8 / 12 | §31.3, §31.4 |
| Event unsubscribe req / resp | 4 / 4 | §31.5 |
| Event query req / resp | 4 / 14 | `0x81 0x03 streamId:u16 eventId:u16 mode:u8 state:u8 periodMs:u16 droppedCount:u32` (§31.6) |
| Event unsubscribe-all req / resp | 2 / 2 | §31.7 |
| Event envelope (fixed part) | 20 | `eventId:u16 formatVersion:u8 flags:u8 streamId:u16 reserved:u16 sequence:u32 timestampUs:u64` (§32) |
| Measurement event payload | 36 | §34 (4×u16 + 6×i32 + quality:u16 + reserved:u16) |
| Local position event payload | 20 | §35 |
| Stream status event payload | 8 | `streamId:u16 state:u8 reason:u8 count:u32` (§36) |

Note: the transport-level type of an event notification is the **generic header**
payload type `0x8010`; `0x81`/`0x10` are *not* part of the event envelope.

## 4. Event validity flags

`MeasurementEvent` flag bits 0–9 are defined by §34 (raw/corrected range,
azimuth, elevation validity, quality valid, and the three ID-valid bits); bits
10–15 are reserved and must be zero. Unavailable numeric fields are encoded as
zero with the corresponding validity bit clear — the codecs never infer validity
from a sentinel value.

## 5. Sessions

§22/§23: `DEFAULT_SESSION = 0x01`, `EXTENDED_SESSION = 0x03`, `SessionId` is one
byte, and an Observer requesting Extended Session is a policy rejection with NRC
`0x22 conditionsNotCorrect`. The codec accepts only the two defined session
values; the role/session decision itself lives in the server core (Phase 2).
`ActivationResponse` (§18.2) carries no session field — session state is only
established by service `0x10`.

## 6. Security Access `0x27`

§25 fixes only the sub-functions (`0x01` request seed, `0x02` send key), the
provider interface (`ISecurityProvider`), and the NRC vocabulary. The seed and key
**widths are a v1 implementation decision**: 4 bytes each
(`kSecuritySeedSize = kSecurityKeySize = 4`), documented here and pinned by golden
vectors.

Failure is reported with the §21.4 NRCs, not with a status byte inside a positive
response:

| Case | NRC |
|---|---|
| Key invalid | `0x35 invalidKey` |
| Too many failed attempts | `0x36 exceedNumberOfAttempts` |
| Retry delay active | `0x37 requiredTimeDelayNotExpired` |
| Security required but not unlocked | `0x33 securityAccessDenied` |
| Security service disabled | `0x11 serviceNotSupported` or `0x7F serviceNotSupportedInActiveSession` (§25) |

Note §21.4: v1 uses `0x35` for `invalidKey` (the earlier draft used `0x34`); the
codec encodes only the v1 table.

The seed/key algorithm stays behind `ISecurityProvider` / client-side identity
provider so it can be replaced without touching the codecs.

## 7. DID records and sizes

§40 defines one layout per DID and no size table, so the single source of truth is
`didRecordSize(Did)` in `dids.hpp`: it returns the expected length of each
fixed-size record and `nullopt` for length-prefixed text records (`0xF002`,
`0xF005`) and TLV containers (`0xF017`, `0xF01F`). A unit test checks every value
against the corresponding golden record.

Validation is a two-step contract:

1. Transport/codec: `did`, record length, payload bounds (§26, §27). A malformed
   request length is NRC `0x13`; an unsupported DID is NRC `0x31`.
2. Record semantics: `did_records.hpp` typed decoders check fields, reserved bits,
   and flag bits. In Phase 2 the server core maps `Did` → `didRecordSize()` plus
   the typed decoder plus access/persistence policy (`didAccess`,
   `didPersistence`).

## 8. `0xF01F` Complete UWB Configuration

§40.22 defines it as a TLV aggregation:

```text
schemaVersion:u16 | entryCount:u16 | repeated ( did:u16 | length:u16 | data[length] )
```

Decisions:

* §40.22 says a v1 configuration "SHOULD contain" `0xF010`–`0xF014`; the codec
  treats that list as the **allowed set** (any other DID → validation failure), so
  a partial apply cannot persist a record the backend cannot interpret.
* Duplicate DIDs → `ProtocolErrorCode::DuplicateEntry`.
* Each entry is validated with its typed §40 decoder, so record size **and**
  internal structure (reserved bits, flag bits) are checked before the server core
  attempts an atomic apply (§40.22 steps 2–4).
* `schemaVersion` must equal the v1 complete-config schema version.
* An empty container is structurally well formed (it simply validates as "no
  entries"); whether the backend accepts an empty write is a Phase 2 policy.

## 9. `0xF017` UWB Miscellaneous Metadata

§40.21: `schemaVersion:u16` followed by `type:u16 | length:u16 | bytes` records,
delimited by the end of the payload (no end-of-list marker); `kTlvHeaderSize = 4`.
v1 types are the §40.21 text types `0x0001` DECA/version text, `0x0002` DLIST,
`0x0003` KLIST. The internal type name is `BackendInfoTlvType` and the container
type is `BackendInfoContainer`; the wire name remains "UWB Miscellaneous
Metadata". Unknown types are preserved verbatim so a decode/encode round trip does
not discard backend output.

## 10. AT command service `0x40`

§30: commands are 1..512 ASCII bytes and MUST NOT contain CR or LF (the Pico
backend appends the module terminator); raw module output — including CRLF — is
carried verbatim. Golden strings come from AT manual **V1.0.7**, e.g.
`AT+GETVER` → `getver software:V1.0.0\r\nOK\r\n`. The command still goes through
the same UWB command manager in Phase 6 — the service is an escape hatch, not a
bypass of UART ownership.

## 11. Time representation

Event timestamps are **Pico monotonic microseconds** (`time_us_64()` class), per
§32; the client records its own host receive timestamp separately. The DID
`0xF00B` Uptime record is also monotonic microseconds. There is no wall-clock sync
service in v1 (`CAP_TIME_SYNC`, capability bit 16, is reserved), so protocol
messages carry no calendar time.

## 12. Open hardware question (blocks Phase 6/8 design lock)

Whether BU03/BU04 modules emit **unsolicited** measurement lines on the TTL UART —
needed to sustain the specification's event-rate target while honoring the plan's
"do not poll `AT+DISTANCE` per iteration" rule — is not answered by the local AT
manuals (V1.0.7 and the ground-truth kit docs) and must be settled on hardware.
The measurement source is therefore an interface in the server core, so the answer
does not change the wire codecs.

## 13. Phase 2 server-core decisions (implemented)

* Server-core DID framework: `didRecordSize()` + typed decode + access/persistence
  policy, mapped to NRCs through one table (`server_errors.hpp`).
* Session lifecycle: activation → optional security → session control → ClientPresent
  refresh / alive-check and idle-timeout policy (§21–§23, §29, §37); every deadline is
  evaluated from `IClock::monotonicUs()`, never from wall-clock time.
* Stream manager: Live drop-oldest with visible counters vs Recording fail-visibly
  (§37), plus high-priority Stream Status events (§36).
* Connection-level `IClock`, `ILogger`, `IConnectionWriter`, `IUwbBackend`,
  `IConfigurationStorage`, and `ISecurityProvider` injection: no transport, socket,
  UART, or filesystem code inside `uwb_server_core`.
* Asynchronous service completion (§21.4 `0x78`) is dispatcher-owned: the dispatcher
  registers a pending request, submits to `IUwbBackend`, supervises the `p2*`
  deadline in `tick()`, and cancels the backend operation on timeout.
* Routine lifecycle: `Idle -> Running -> Stopping -> Completed/Failed/Cancelled`;
  `IUwbBackend` operations are owned by a connection id so §9.4 cleanup
  (`cancelOperations(owner)`, `stopAllForConnection`) is possible without a global
  "current owner".
* DID record validation NRCs follow §27: `0x13` for structural errors, `0x31` for
  out-of-range values (§27 wording; see §15 below).

## 14. Server-core TX priority classes (Phase 2)

Specification §38 defines three logical transmission classes and §53 only two
physical per-connection queues. Mapping used by `ConnectionTxQueue`:

| §38 class | `TxPriority` | physical queue (§53) | contents |
|---|---|---|---|
| High | `High` | high-priority TX (8 frames) | service responses (including async `0x78` final responses and AT/routine results), ACK/NACK, GenericHeaderNack, activation/alive/session/security messages, connection management, **Stream Status** events |
| Normal | `Normal` | normal-priority TX (8 frames) | non-critical status events (`UwbStatus`, `DiagnosticLog`) |
| Streaming | `Low` | normal-priority TX, drained last | range/angle/localization/sensor samples (`UwbMeasurement`, `UwbLocalPosition`, `SensorData`) |

Consequences that are normative and were verified by unit tests:

* SRV-005 / §38: a service response is never queued behind a backlog of
  streaming samples (`a service response overtakes a queued measurement backlog`).
* Stream Status SHALL NOT share the drop policy of live measurements (§38); it is
  pushed High.
* Low-priority frames live in the normal queue and are drained after Normal-class
  frames so a third physical queue is not needed on RP2040.

## 15. Decode-error NRC rule for services (Phase 2)

§27 states the general rule: structurally invalid data is `0x13`, a syntactically
valid value outside the allowed range is `0x31`. `ServiceDispatcher` applies it to
every service PDU decode failure (DID read/write, Session Control, Routine
Control, Event Control, Raw AT), so an unknown DID, an unknown routine id, an
unknown session id, or an over-long AT command is answered `0x31` rather than
`0x13`.

## 16. Security seed lifetime (Phase 2)

`SecurityAccess` follows §25 with a v1 4-byte seed/key. A seed is single-use: a
failed `sendKey`, a session transition, or a disconnect invalidates it, so a
client must request a fresh seed (`0x27 0x01`) before retrying (`0x35`, `0x36`,
`0x37` follow §21.4).

