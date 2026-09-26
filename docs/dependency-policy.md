# Dependency policy

Implementation plan §2.1 and §3 require the dependency layers below. CI and the
local script enforce them structurally: a target only links what is listed here.

| Target | Allowed dependencies | Forbidden |
|---|---|---|
| `uwb::protocol` (`shared/protocol/`) | C++20 standard library only | Pico SDK, Asio, sockets, UART, filesystem, GUI, Catch2 (except in tests) |
| `uwb::domain` (`shared/domain/`) | `uwb::protocol`, standard library | everything else |
| `uwb::server_core` (`server-core/`) | `uwb::protocol`, `uwb::domain`, standard library | Asio, sockets, Pico SDK, concrete transports, storage backends |
| `uwb::client_core` (`client-core/`) | `uwb::protocol`, `uwb::domain`, standard library | Asio, sockets, GUI |
| `uwb_asio` (INTERFACE) | standalone Asio (`ASIO_STANDALONE`), `uwb::protocol` | Boost.Asio, Pico SDK |
| `uwb_simulator` (`simulator/`) | `uwb::server_core`, `uwb_asio` | Pico SDK |
| `uwbctl` (`apps/cli/`) | `uwb::client_core`, `uwb_asio` | GUI libraries |
| `pico/` firmware | Pico SDK, `uwb::protocol`, `uwb::domain`, vendored LittleFS/libcrc | Asio host networking, host-only libraries |
| `uwb_unit_tests`, `uwb_integration_tests` | Catch2 + the target under test | — |
| `uwb_hil_tests` (opt-in) | Catch2, `uwb::protocol`, `uwb::domain`, Asio transport | built only with `-DUWB_ENABLE_HIL_TESTS=ON` |

## Structural rules

* Server core depends on **interfaces** (`ITransport`, `IConnection`, `IClock`,
  `ILogger`, `IStorage`, `IUwbBackend`, `ISecurityProvider`), never on a concrete
  transport or driver. Concrete implementations are injected by the host
  application (simulator, CLI) or by the Pico firmware.
* Transport adapters convert bytes ↔ `FrameParser`/`encodeFrame` calls only; they
  contain no protocol semantics.
* `shared/` must compile for both the host and RP2040 unchanged — verified by the
  `pico-w` preset building the same `uwb_protocol` sources.
* Vendored third-party code lives in `external/` with a pinned revision
  (`external/README.md`, `tools/refresh_libcrc.sh`).

## Checks

```bash
# protocol library must link nothing but the standard library
grep -n "target_link_libraries" shared/protocol/CMakeLists.txt   # no matches expected

# protocol sources must not reference platform/network headers
grep -rn "asio\|sys/socket\|arpa/inet\|unistd\|pico/\.h\|pico_/std::" shared/protocol/
```
