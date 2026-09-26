# uwb-system

C++20 implementation of the UWB Pico W client/server system described in
[`../uwb_pico_client_server_specification.md`](../specification.md) and
[`../uwb_pico_client_server_implementation_plan.md`](../implementation_plan.md).

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

Test labels: `[unit]` (243 cases: protocol golden vectors, server core, client
core), `[integration]` (50 cases: real UDP/TCP loopback against the host
simulator), `[simulator]`, `[hil]` (manual, hardware required, never in CI).
The protocol golden vectors are generated from the specification tables, never
from the C++ implementation:

```bash
tests/golden/regenerate.sh   # regenerate tests/unit/gen/golden_vectors.hpp
tests/golden/verify.sh       # CI check: committed vectors == regeneration
```

Everything above also runs in `ci/run_local_ci.sh all` (debug + ASan/UBSan).

Protocol decisions and specification reconciliations are recorded in
[docs/protocol_decisions.md](docs/protocol_decisions.md); phase progress in
[docs/phases.md](docs/phases.md).

Sanitizer build (parser/replayer hardening):

```bash
cmake --preset host-asan
cmake --build --preset host-asan
ctest --preset host-asan
```

## Host simulator and `uwbctl` client

```bash
./build/host-debug/simulator/uwb_simulator --device-name anchor1 --board-id 1,2,3,4,5,6,7,8
./build/host-debug/simulator/uwb_simulator --device-name anchor2 --board-id 2,2,3,4,5,6,7,8

./build/host-debug/apps/cli/uwbctl discover
./build/host-debug/apps/cli/uwbctl connect anchor1 anchor2 --diagnostics
./build/host-debug/apps/cli/uwbctl config get anchor1
./build/host-debug/apps/cli/uwbctl stream range anchor1 --duration 5000
```

`docs/simulator.md` documents the simulator options and fault injection,
`docs/cli.md` documents every `uwbctl` command, the threading model, and the
manual acceptance run `tools/cli_smoke.sh`.

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
