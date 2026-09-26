# Host simulator (`uwb_simulator`)

Implementation plan §20–§23. The simulator runs the **real** `uwb_server_core`
protocol state machine as a PC process so client development (Phase 4/5) and
integration testing do not need hardware.

## 1. Structure

```
simulator/
  include/uwb/simulator/simulator_config.hpp    SimulatorOptions + CLI parser
  include/uwb/simulator/host_services.hpp       SystemClock, ManualClock,
                                                MemoryConfigStorage,
                                                StaticSecurityProvider, ConsoleLogger
  include/uwb/simulator/simulated_backend.hpp   SimulatedUwbBackend (IUwbBackend)
  include/uwb/simulator/simulator_runtime.hpp   SimulatorRuntime (no socket code)
  include/uwb/simulator/asio_server.hpp         SimulatorServer (Asio glue)
  src/main.cpp                                  CLI entry point
```

* `SimulatorRuntime` wires `ServerCore` together with host implementations of
  `IClock`, `IConfigurationStorage`, `ISecurityProvider` and `IUwbBackend`. It
  contains **no** transport code: `onConnect()`, `onBytes()`, `onDisconnect()` and
  `tick()` are the same calls the Phase 6 Pico lwIP glue will make.
* `SimulatorServer` is the only file that touches sockets: UDP discovery
  responder (§8), TCP acceptor, per-connection async read/write, and a 10 ms timer
  that drives `SimulatorRuntime::tick()` (`kSimulatorTickIntervalMs`).
* `SimulatedUwbBackend` models the solicited-command rules of specification §48:
  exactly one exchange on the simulated UART line, a bounded command queue,
  per-command timeout, optional inter-command gap, and injectable fault classes.
* Nothing in `shared/` or `server-core/` is reimplemented for the simulator.

## 2. Build and run

```bash
cmake --preset host-debug && cmake --build --preset host-debug
./build/host-debug/simulator/uwb_simulator --verbose
# discovery on udp/13401, protocol on tcp/13401, Ctrl-C (SIGINT/SIGTERM) stops cleanly
```

Ports `0` ask the OS for ephemeral ports, which is what the integration tests use
so they can never collide with a real device.

## 3. Options

| Option | Meaning |
|---|---|
| `--board-id <1,2,3,4,5,6,7,8>` or `<16 hex digits>` | Board unique id; the Device UUID is UUIDv5 of its lowercase hex (§6.2) |
| `--device-name <name>` | Device name, max 32 bytes (§17) |
| `--logical-address <hex>` | Device logical address (default `0x1001`, server range `0x1000`–`0x1DFF`) |
| `--udp-port <port>` / `--tcp-port <port>` | Discovery / protocol ports (default `13401`, `0` = ephemeral) |
| `--bind <address>` | Bind address (default `0.0.0.0`) |
| `--known-mask <mask>` | Known capability mask reported in the capability DIDs (§39) |
| `--detected-mask <mask>` | Detected capability mask; effective mask = `(known \| override-on) & ~override-off` (§41) |
| `--override-on <mask>` / `--override-off <mask>` | Capability override record (§40.9) |
| `--period-ms <n>` | Measurement emission period (default 100 ms) |
| `--tags <n>` / `--anchors <n>` | Simulated tag/anchor counts (network id metadata in §34 events) |
| `--no-pdoa` | Suppress PDOD azimuth/elevation fields in measurement events |
| `--local-position` | Also emit `0x0102` local position events (§35) |
| `--range-mm <n>` / `--noise-mm <n>` | Base range and noise amplitude in millimetres |
| `--seed <n>` | RNG seed for measurement generation (fixed by default → reproducible) |
| `--backend-latency-ms <n>` | Simulated UWB command latency (§48) |
| `--backend-gap-ms <n>` | Simulated inter-command gap (§48.3) |
| `--security` / `--security-key <hex>` | Enable Security Access (service `0x27`) and the accepted key (§25) |
| `--no-raw-at` | Disable the raw AT service (`0x40`) |
| `--p2-server-ms <n>` | Reported `p2ServerMaxMs` (§52) |
| `--p2-star-10ms <n>` | Reported `p2*ServerMax` in 10 ms units (§52) |
| `--deterministic` | Stepped logical clock + no measurement jitter (see §5) |
| `--fault-timeout` | Next UWB command never answers → `0x72` at `p2*` |
| `--fault-parse-error` | Next UWB command answers malformed → `0x72` |
| `--fault-unsupported` | Next UWB command answers "not supported" → `0x31` |
| `--backend-full` | UWB command queue reports full → `0x21` |
| `-v`, `--verbose` | Log protocol activity |
| `-h`, `--help` | Usage |

Fault knobs are one-shot: they are armed at startup and cleared when consumed, so
they can be used from the CLI as well as from tests (they are configured through
`SimulatorOptions` so a test never mutates the backend while the simulator thread
is running).

## 4. Threading contract

`ServerCore` is single threaded. Everything that mutates it runs on the simulator
event-loop thread (`onConnect` / `onBytes` / `onDisconnect` / `tick`). Test code
and the CLI therefore observe the runtime through the **atomic snapshots**
published by `SimulatorRuntime` (`connectionCount()`,
`measurementEventsPublished()`, `simulatedResets()`) and never reach into
`core()` while the loop is running.

Connection lifetime in the Asio layer: each connection is a
`shared_ptr<TcpConnection>` with `enable_shared_from_this`, because pending
`async_read_some` / `async_write` completion handlers reference it. `close()`
erases the map entry first, then sets `closing` and closes the socket, so an
outstanding completion sees `closing` and returns without touching a dead object.
`requestStop()` is the thread-safe shutdown entry (signal handlers, test fixture);
full teardown runs only after the worker thread has joined.

## 5. Deterministic mode (plan §22.3)

Measurement values are generated by a seeded `std::mt19937` and depend only on the
sample index, so two runs with the same options produce byte-identical event
payloads.

`--deterministic` additionally replaces the wall clock with a `ManualClock` that
advances by exactly `kSimulatorTickIntervalMs` (10 ms) per simulator tick. Timeout
supervision (`p2`, `p2*`, TCP idle, alive check), stream pacing, and measurement
scheduling then advance in fixed logical quanta and are independent of host
scheduling jitter. `SimulatorRuntime::manualClock()` exposes it for tests that want
to push logical time directly.

## 6. Integration harness

`tests/integration/test_transport.hpp` provides the harness used by the
`[integration]` tests:

* `SimulatorFixture` — starts a `SimulatorRuntime` + `SimulatorServer` on a worker
  thread with ephemeral ports and stops it deterministically.
* `TcpClient` — non-blocking Asio client with its own `io_context` (never the
  simulator's), incremental `FrameParser`, `waitForResponse()`, `waitForPending()`,
  `waitForFinal()`, `waitForClose()`.
* `discover()` — UDP Device Identification Request/Response.
* `WireFrame`, `servicePduOf()`, `transactionOf()`, `nrcOf()`, `nackOf()`,
  `applicationFrame()`, `activationFrame()`, `fastOptions()`.

```bash
ctest --preset host-debug -L integration   # 29 tests
ctest --preset host-asan  -L integration   # same suite under ASan/UBSan
```

## 7. What the simulator does *not* model

* It answers raw AT with deterministic synthetic text (`AT+GETVER` matches the
  AT manual V1.0.7 shape); it is not an AT parser for the real module grammar.
* UWB geometry is synthetic: ranges/angles come from the seeded RNG, no real
  anchors, tags, or radio behaviour.
* Persistent storage is `MemoryConfigStorage` (the two-phase stage/commit rule of
  §51 is preserved, but nothing is written to a filesystem). LittleFS arrives with
  Phase 7 on the Pico.
* `0x11` device reset is simulated as a host-side "reset executed" counter; there
  is no MCU restart.
