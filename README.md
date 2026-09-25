# uwb-system

C++20 implementation of the UWB Pico W client/server system described in
[`../specification.md`](../specification.md) and
[`../implementation_plan.md`](../implementation_plan.md).

## Layout

| Directory | Contents |
|---|---|
| `shared/protocol/` | Dependency-free wire protocol, TLV, UUIDv5, CRC32 helpers |
| `shared/domain/` | Device IDs, device info, discovery/capability/config models, errors |
| `server-core/` | Transport-neutral server core (sessions, discovery, routines, state, persistence) |
| `simulator/` | Host simulator that emulates Pico firmware, UWB devices, and networking |
| `client-core/` | Client core (discovery, sessions, state, logging) |
| `apps/cli/` | `uwbctl` command line interface |
| `pico/` | RP2040/Pico W firmware (Pico SDK build only) |
| `integrations/` | Recorder, aggregate TCP export, ROS 2 integration |
| `tests/` | `unit/`, `integration/`, `hil/` tests |
| `external/` | Pinned vendored third-party sources (libcrc) |

## Host build

```bash
cmake --preset host-debug
cmake --build --preset host-debug
ctest --preset host-debug
```

Sanitizer build (parser/replayer hardening):

```bash
cmake --preset host-asan
cmake --build --preset host-asan
ctest --preset host-asan
```

## Pico firmware build

Requires the Pico SDK (`PICO_SDK_PATH`, e.g. `/home/chris/pico`) and
`arm-none-eabi-gcc`:

```bash
cmake --preset pico-w
cmake --build --preset pico-w
```

## Hardware-in-the-loop tests

```bash
cmake --preset host-debug -DUWB_ENABLE_HIL_TESTS=ON \
      -DHIL_SERIAL_DEVICE=/dev/ttyUSB0 -DHIL_PICO_HOST=192.168.1.20
cmake --build --preset host-debug
ctest --preset host-debug -L hil
```

See [docs/build.md](docs/build.md) for dependency notes.
