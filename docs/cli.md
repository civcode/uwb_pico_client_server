# `uwbctl` host CLI (Phase 4)

`uwbctl` is the PC-side command line client for the UWB system. It is the
reference consumer of `uwb_client_core` + `uwb_client_net` and the manual
acceptance tool for §29 (Milestone B).

```bash
cmake --preset host-debug
cmake --build --preset host-debug            # builds uwbctl
./build/host-debug/apps/cli/uwbctl --help
```

## Threading model (specification §55)

```
CLI / application thread                single I/O thread
------------------------                -------------------
parse argv, print results               Asio io_context
ClientSession.post(lambda)  ------>     ApplicationController, ConnectionManager,
                                        DiscoveryClient, per-connection state
                                        timers, parsers, transaction matcher
waitPop(...)                <------     NotificationQueue (bounded, drop-oldest)
```

* All client-core state lives on the I/O thread. The CLI never touches a
  connection directly; it posts work through `ClientSession` and marshals
  results back with `post()` + callback completion, or with
  `ClientSession::invoke()` for a synchronous read.
* Events (`DeviceSeen`, `DeviceStateChanged`, `DeviceError`, `Measurement`,
  `StreamStateChanged`) reach the CLI through `session.notifications()`.
* A slow consumer fills the bounded notification queue. The queue drops the
  oldest notification and counts the drop; discovery, ClientPresent, and
  request/response processing keep running (`--capacity 4 --diagnostics`
  demonstrates this).

## Endpoint selection (specification §58)

Device arguments resolve in this order:

1. device name
2. UUID (`xxxxxxxx-xxxx-5xxx-8xxx-xxxxxxxxxxxx`)
3. `host:port` — sends a one-shot unicast `DeviceIdRequest` probe to that
   endpoint first, then connects to the port from the response.

`--target ip:port` restricts or extends discovery. `--broadcast` adds
`255.255.255.255` and `127.255.255.255`; `--no-broadcast` disables the default
broadcast fallback. Without any `--target` the client probes the default
broadcast targets on the default port `13401`.

## Session, role, and security rules

| Requirement | Enforced by | Symptom when missing |
|---|---|---|
| Control role for activation | server-core §9.2 | activation response with `role none` / NRC 0x11 |
| Extended Session for DID writes, raw AT, routines with side effects | server-core §22.3 / §28 / §30 | NRC 0x22 |
| Control role **and** Extended Session for `Pico Device Reset` | server-core §24 | NRC 0x22 |
| Security Access before Extended Session on a secured device | server-core §23 + §25 | NRC 0x33 |

`uwbctl` requests the Extended Session automatically for `config set`,
`config save`, `at`, `routine`, `reset`, and `security`, and authenticates
first when `--key <hex>` is present:

```bash
uwbctl at  anchor1 "AT+AUTO_CAL" --key 0xdeadbeef   # 0x27 seed+key, then 0x10, then 0x31
uwbctl config set anchor1 --rate 850 --key 0xdeadbeef
```

```bash
# discovery, connect by address, and connect the whole fleet in one process
uwbctl discover --target 192.168.1.50:13401 --target 192.168.1.51:13401
uwbctl connect 192.168.1.50:13401 --diagnostics
uwbctl connect anchor1 anchor2 tag1 --diagnostics
uwbctl stream range anchor1 --duration 10000 --diagnostics
```

With a wrong key the server answers NRC `0x35` (invalid key); with no key the
Extended Session request is refused with NRC `0x33`.

## Commands

```
uwbctl version
uwbctl discover                       [--target ip:port] [--broadcast] [--no-broadcast]
uwbctl list                           [--target ip:port]
uwbctl monitor  [device ...]          [--duration ms] [--diagnostics]
uwbctl connect  <device ...>          [--role control|observer] [--follow] [--diagnostics]
uwbctl disconnect <device>
uwbctl info     <device>
uwbctl diagnostics
uwbctl did get  <device> <name|0xF00B|...>     name, address, uptime, wifi-rssi,
                                               latest-distance, 0xF015, ...
uwbctl config get  <device>                    Complete UWB configuration (0xF01F)
uwbctl config set  <device> [--id N] [--role N] [--channel N] [--rate N]
                            [--dlist N] [--klist N] [--network N] [--anchor N]
                            [--filter on|off] [--pdoa-offset N] [--range-offset N]
                            [--tag-capacity N] [--antenna-delay N] [--twr-flags N]
                            [--did 0xF009=<hex>]
uwbctl config save <device>                    SaveUwbConfiguration routine
uwbctl session  <device> <extended|default>
uwbctl stream range <device> [--mode live|recording] [--duration ms] [--keep] [--diagnostics]
uwbctl stream stop  <device>
uwbctl routine <device> <start-anchoring|save-config|0x0204> [--options hex]
uwbctl routine stop <device>
uwbctl at     <device> "AT+ID"
uwbctl reset  <device> <soft|hard>
uwbctl security <device> <hex-key>
```

Global options: `--port <n>`, `--target ip:port` (repeatable), `--broadcast`,
`--no-broadcast`, `--role <control|observer>`, `--key <hex>`,
`--connect-timeout ms` (3000), `--request-timeout ms` (3000),
`--routine-timeout ms` (15000), `--duration ms` (5000),
`--probe-interval ms` (1000), `--capacity n` (notification queue, 512),
`--reconnect on|off`, `--diagnostics`, `--verbose`.

`config set --rate` and friends use the AT unit convention from the BU03/BU04
manual: `--rate` is a data rate index such as `850`, not a bitrate symbol;
`--range-offset` is millimetres, `--pdoa-offset` is the raw signed offset
value stored in the configuration DID.

## Exit codes

| Code | Meaning |
|---|---|
| `0` | command succeeded |
| `1` | client, protocol, or device error (message on `stderr`) |
| `2` | bad command line |

## Manual acceptance run

`tools/cli_smoke.sh` starts four host simulators (three plain, one secured on
port `13404` with `--security-key 0xdeadbeef`) and drives the CLI through
discovery, concurrent connects to three devices, DID reads, configuration
read/write/save, session change, raw AT, routine control, live and recording
measurement streaming, a slow-consumer monitor (`--capacity 4`), disconnect,
Security Access (accepted and rejected), and Device Reset:

```bash
bash tools/cli_smoke.sh
```

Example slow-consumer evidence (`--diagnostics`):

```
connections               3 (active 3)
requests                  3 sent, 3 matched
timeouts                  0
protocol errors           0
tx queue drops            0
notification drops        11
reconnect attempts        0
```

Drops appear only in the notification queue, which is the §56/§60 requirement.

## Known limits

* `uwbctl` is one-shot per invocation: each command rediscovers (or probes the
  explicit target) and disconnects on exit. Use `connect --follow` or
  `monitor` for a long-lived session.
* Security Access uses the v1 fixed-key handshake (seed then configured key);
  a real challenge-response algorithm is a firmware-side decision (§25 leaves
  it open).
* No interactive mode and no GUI yet (Phase 7/8).
